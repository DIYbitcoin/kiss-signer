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
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont

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

MENLO = "/System/Library/Fonts/Menlo.ttc"


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    """A mono face. Menlo, not the site's Ioskeley: PIL cannot read woff2, and
    the shape of the letters matters less here than that they are monospaced."""
    return ImageFont.truetype(MENLO, size, index=1 if bold else 0)


def corner_brackets(d: ImageDraw.ImageDraw, box, arm=26, w=2, colour=EDGE):
    """The reticle the device frames a target with, and the site frames its
    panels with. Four corners, drawn as eight strokes."""
    x0, y0, x1, y1 = box
    for cx, cy, dx, dy in ((x0, y0, 1, 1), (x1, y0, -1, 1), (x0, y1, 1, -1), (x1, y1, -1, -1)):
        d.line([(cx, cy), (cx + arm * dx, cy)], fill=colour, width=w)
        d.line([(cx, cy), (cx, cy + arm * dy)], fill=colour, width=w)


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

    card = build()

    if args.check:
        if not OUT.exists():
            sys.exit(f"{OUT.relative_to(ROOT)} is missing; run this script")
        if Image.open(OUT).convert("RGB").tobytes() != card.tobytes():
            sys.exit(f"{OUT.relative_to(ROOT)} is stale; re-run "
                     f"python3 assets/generators/og_card.py")
        print(f"og card: {OUT.relative_to(ROOT)} matches the current home screen")
        return

    OUT.parent.mkdir(parents=True, exist_ok=True)
    card.save(OUT, "PNG", optimize=True)
    print(f"wrote {OUT.relative_to(ROOT)}  ({W}x{H}, {OUT.stat().st_size / 1024:.0f} KB)")


if __name__ == "__main__":
    main()
