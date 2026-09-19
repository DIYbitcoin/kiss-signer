#!/usr/bin/env python3
"""GAME OVER screen for FRUIT ISLAND (LANDSCAPE 800x480). Emits two LVGL sprites:
  img_gameover  (800x480 RGB565)  - synthwave backdrop, logo, card frame, PLAY AGAIN, fruit.
  img_newbest   (RGB565A8)        - the rotated NEW BEST! ribbon (shown when beaten).
Dynamic score/best numbers are overlaid as LVGL labels; card/label coords stay in sync with
show_game_over() in main.c (over_lbl y=240, best y=316, newbest y=128).

Every board, one drawing. Every length below is the wide 800x480 number and X()/Y()
floor it onto the canvas exactly as SX()/SY() in main/kiss_board.h do, so the baked
card, pill and ribbon sit under the live labels and hit boxes on each board.
`--board ws35` draws the same picture at 480x320 into main/gameover_img_ws35.c, and
`--board jc1060` at 1024x600 into main/gameover_img_jc1060.c; each defines the same
symbols under the wide header. Without the flag the output is the wide file, byte
for byte. boards.py lists the boards."""
import argparse
import os
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scene import synthwave_scene, fill_holes
from boards import BOARDS, DESIGN_W, DESIGN_H, per_board

ap = argparse.ArgumentParser(description="GAME OVER backdrop and NEW BEST ribbon")
ap.add_argument("--board", choices=sorted(BOARDS), default="guition",
                help="guition draws 800x480 (default); ws35 draws 480x320; "
                     "jc1060 draws 1024x600")
BOARD = ap.parse_args().board
DW, DH = DESIGN_W, DESIGN_H              # the design canvas every number below is written for
W, H = BOARDS[BOARD].w, BOARDS[BOARD].h
SUFFIX = BOARDS[BOARD].suffix
# main.c's KISS_NARROW: the card's live rows (OVER_*) are pixels on a narrow
# board and scaled design numbers on a wide one, and the card follows them.
NARROW = BOARDS[BOARD].narrow
CX = W // 2
ROUND = "/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EMO = os.path.join(ROOT, "assets/emoji")
OUT = os.path.join(ROOT, "main")


def X(v):
    """An x, a width or a square thing's side: floored like SX()."""
    return v * W // DW


def Y(v):
    """A y or a height: floored like SY()."""
    return v * H // DH


def PT(v):
    """A font size: the vertical scale, never under 10 px."""
    return max(10, v * H // DH)


def LW(v):
    """A stroke or line width: the vertical scale rounded, never under 1 (3 -> 2)."""
    return max(1, (2 * v * H + DH) // (2 * DH))


def BL(v):
    """A blur radius: the vertical scale, unrounded."""
    return v * H / DH


HZ = Y(318)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def seal_counters(m):
    """The 3.5in's title plate, with every pixel the outside cannot reach made
    solid. fill_holes fills only the enclosed pixels under half coverage; at 2/3
    scale the O kept a ring of part-covered pixels and the shadow drew through
    it as a dotted ellipse inside the letter. Scaled up to the 7in the O drew
    the same ellipse, and its plate is sealed too. The 4.3in's plate stays
    fill_holes' alone, because its picture is held byte for byte (SEAL below)."""
    a = np.array(m)
    free = a < 255
    reach = np.zeros_like(free)
    reach[0, :] = free[0, :]; reach[-1, :] = free[-1, :]
    reach[:, 0] |= free[:, 0]; reach[:, -1] |= free[:, -1]
    while True:
        grow = reach.copy()
        grow[1:, :] |= reach[:-1, :]
        grow[:-1, :] |= reach[1:, :]
        grow[:, 1:] |= reach[:, :-1]
        grow[:, :-1] |= reach[:, 1:]
        grow &= free
        if (grow == reach).all():
            break
        reach = grow
    a[free & ~reach] = 255
    return Image.fromarray(a)


# Whether the title plate is sealed as well as filled: see seal_counters.
SEAL = per_board(BOARD, {"guition": False, "ws35": True, "jc1060": True})


# ---- synthwave scene (same hero as the menu), blurred + dimmed so the card pops ----
stops = [(0.0, (24, 16, 58)), (0.40, (78, 40, 100)), (0.56, (168, 72, 96)),
         (0.63, (255, 150, 74)), (0.665, (236, 120, 72)), (0.71, (26, 78, 92)),
         (1.0, (7, 26, 38))]
sky = np.zeros((H, W, 3), np.uint8)
for y in range(H):
    f = y / (H - 1)
    for i in range(len(stops) - 1):
        a, b = stops[i], stops[i + 1]
        if a[0] <= f <= b[0]:
            sky[y, :] = lerp(a[1], b[1], (f - a[0]) / (b[0] - a[0] + 1e-9))
            break
img = Image.fromarray(sky, "RGB").convert("RGBA")
yy, xx = np.mgrid[0:H, 0:W].astype(float)
img = synthwave_scene(img, W, H, CX, HZ, xx, yy)
img = img.filter(ImageFilter.GaussianBlur(BL(5)))
img = Image.alpha_composite(img, Image.new("RGBA", (W, H), (6, 10, 26, 135)))
vg = (np.clip((np.sqrt((xx - CX) ** 2 + (yy - Y(240)) ** 2) / X(480) - 0.4) / 0.6, 0, 1) * 175).astype(np.uint8)
vv = np.zeros((H, W, 4), np.uint8); vv[..., 3] = vg
img = Image.alpha_composite(img, Image.fromarray(vv, "RGBA"))


def logo_line(txt, size, cy, top, bot):
    PAD = Y(200)
    f = ImageFont.truetype(ROUND, PT(size))
    tmp = Image.new("RGBA", (W, PAD), (0, 0, 0, 0)); td = ImageDraw.Draw(tmp)
    bb = td.textbbox((0, 0), txt, font=f, stroke_width=LW(9))
    tw, th = bb[2] - bb[0], bb[3] - bb[1]
    x = (W - tw) / 2 - bb[0]; y = Y(28)
    # cream backing = the stroked text itself (original look); fill_holes only
    # plugs enclosed apertures (A's triangle) so no shadow fragment shows through
    plate = Image.new("L", (W, PAD), 0)
    ImageDraw.Draw(plate).text((x, y), txt, font=f, fill=255, stroke_width=LW(9), stroke_fill=255)
    plate = fill_holes(plate)
    if SEAL:
        plate = seal_counters(plate)
    sh = Image.new("RGBA", (W, PAD), (0, 0, 0, 0))
    sh.paste((0, 0, 0, 160), (0, Y(7)), plate)
    tmp.alpha_composite(sh.filter(ImageFilter.GaussianBlur(BL(4))))
    tmp.paste((252, 244, 220, 255), (0, 0), plate)
    mask = Image.new("L", (W, PAD), 0); ImageDraw.Draw(mask).text((x, y), txt, font=f, fill=255)
    grad = np.zeros((PAD, W, 3), np.uint8)
    for yy3 in range(PAD):
        grad[yy3, :] = lerp(top, bot, np.clip((yy3 - y) / th, 0, 1))
    tmp.paste(Image.fromarray(grad, "RGB").convert("RGBA"), (0, 0), mask)
    img.alpha_composite(tmp, (0, Y(cy) - Y(100)))


# The title's design size and centre y. The 3.5in's type scales by 2/3 and its
# width by 3/5, so the scaled title ran 6 px into the MENU pill and its rim sat
# 4 px under the top edge; there it is 40 px, its rim 8 px down, ending 14 px
# short of the pill. The 7in starts from the wide row: one line fits landscape
# with no top clip, and its type scales less than its width does.
TITLE = per_board(BOARD, {"guition": (66, 82), "ws35": (60, 80), "jc1060": (66, 82)})
logo_line("GAME OVER", *TITLE, (255, 209, 96), (231, 116, 44))

# ---- score card (frame + SCORE label + divider; numbers overlaid at runtime) ----
card = Image.new("RGBA", (W, H), (0, 0, 0, 0)); cd = ImageDraw.Draw(card)
x0, y0, x1, y1 = CX - X(165), Y(202), CX + X(165), Y(352)
if NARROW:
    # The card's rows in pixels, one stack with main.c's OVER_* numbers: SCORE,
    # the score in Montserrat 40 (digits 153..181), the rule at 188, BEST in
    # Montserrat 28 (195..215). Scaled, the score sat on the rule and BEST ran
    # through the bottom border. The top stays 8 px under the NEW BEST ribbon.
    y0, y1 = 124, 226
sh = Image.new("RGBA", (W, H), (0, 0, 0, 0))
ImageDraw.Draw(sh).rounded_rectangle([x0, y0 + Y(10), x1, y1 + Y(10)], X(28), fill=(0, 0, 0, 130))
card = Image.alpha_composite(card, sh.filter(ImageFilter.GaussianBlur(BL(7)))); cd = ImageDraw.Draw(card)
cd.rounded_rectangle([x0, y0, x1, y1], X(28), fill=(30, 22, 18, 235), outline=(236, 184, 84), width=LW(4))
fs = ImageFont.truetype(ROUND, PT(28))
bb = cd.textbbox((0, 0), "SCORE", font=fs, stroke_width=LW(2))
if NARROW:
    cd.text(((W - (bb[2] - bb[0])) / 2, y0 + 8 - bb[1]), "SCORE", font=fs, fill=(236, 200, 120), stroke_width=LW(2), stroke_fill=(0, 0, 0))
    cd.line([x0 + X(44), 188, x1 - X(44), 188], fill=(236, 184, 84, 140), width=LW(2))
else:
    cd.text(((W - (bb[2] - bb[0])) / 2, y0 + Y(14)), "SCORE", font=fs, fill=(236, 200, 120), stroke_width=LW(2), stroke_fill=(0, 0, 0))
    cd.line([x0 + X(44), y0 + Y(104), x1 - X(44), y0 + Y(104)], fill=(236, 184, 84, 140), width=LW(2))
img.alpha_composite(card)

# ---- PLAY AGAIN button ----
btn = Image.new("RGBA", (W, H), (0, 0, 0, 0))
bx0, by0, bx1, by1 = CX - X(168), Y(372), CX + X(168), Y(444)
sha = Image.new("RGBA", (W, H), (0, 0, 0, 0))
ImageDraw.Draw(sha).rounded_rectangle([bx0, by0 + Y(8), bx1, by1 + Y(8)], X(38), fill=(0, 0, 0, 130))
btn = Image.alpha_composite(btn, sha.filter(ImageFilter.GaussianBlur(BL(6)))); bd = ImageDraw.Draw(btn)
bd.rounded_rectangle([bx0, by0, bx1, by1], X(38), fill=(58, 178, 78), outline=(252, 230, 150), width=LW(4))
fb = ImageFont.truetype(ROUND, PT(40))
tb = bd.textbbox((0, 0), "PLAY AGAIN", font=fb)
bd.text(((W - (tb[2] - tb[0])) / 2, by0 + Y(14)), "PLAY AGAIN", font=fb, fill=(255, 255, 255),
        stroke_width=LW(2), stroke_fill=(30, 110, 50))
img.alpha_composite(btn)

# ---- fruit accents flanking the title (cherries nudged clear of the MENU pill) ----
# (emoji, design position, design size). The 3.5in's are off its title, one
# each side of the ribbon: the melon sat on the G, and the cherries there keep
# 19 px under the MENU pill and 24 off the ribbon. The 7in's start as the wide ones.
ACCENTS = per_board(BOARD, {
    "guition": [("watermelon", (150, 56), 90), ("cherries", (545, 96), 88)],
    "ws35": [("watermelon", (144, 100), 90), ("cherries", (568, 96), 88)],
    "jc1060": [("watermelon", (150, 56), 90), ("cherries", (545, 96), 88)],
})
for nm, pos, sz in ACCENTS:
    fr = Image.open(f"{EMO}/{nm}.png").convert("RGBA"); fr.thumbnail((X(sz), X(sz)), Image.LANCZOS)
    img.alpha_composite(fr, (X(pos[0]), Y(pos[1])))

# ---- MENU button (top-right, card style; inset for panel overscan) ----
# hit region lives in main.c game_tick ST_OVER tap handling -- keep in sync
# The 3.5in's is out in the corner, level with its title, in pixels: where it
# scaled to, the pill covered the R. Any tap off PLAY AGAIN goes to the menu, so
# no hit box moves with it. The 7in's is the wide pill scaled.
mx0, my0, mx1, my1 = per_board(BOARD, {
    "guition": (X(612), Y(30), X(752), Y(84)),
    "ws35": (380, 11, 464, 45),
    "jc1060": (X(612), Y(30), X(752), Y(84)),
})
mb = Image.new("RGBA", (W, H), (0, 0, 0, 0))
msh = Image.new("RGBA", (W, H), (0, 0, 0, 0))
ImageDraw.Draw(msh).rounded_rectangle([mx0, my0 + Y(6), mx1, my1 + Y(6)], X(27), fill=(0, 0, 0, 120))
mb = Image.alpha_composite(mb, msh.filter(ImageFilter.GaussianBlur(BL(5)))); md = ImageDraw.Draw(mb)
md.rounded_rectangle([mx0, my0, mx1, my1], X(27), fill=(30, 22, 18, 235), outline=(236, 184, 84), width=LW(3))
fm = ImageFont.truetype(ROUND, PT(26))
tb = md.textbbox((0, 0), "MENU", font=fm)
md.text(((mx0 + mx1 - (tb[2] - tb[0])) / 2 - tb[0], (my0 + my1 - (tb[3] - tb[1])) / 2 - tb[1]),
        "MENU", font=fm, fill=(236, 200, 120))
img.alpha_composite(mb)

base = img.convert("RGB")  # firmware image (no dynamic numbers, no newbest)

# ---- NEW BEST! ribbon: clean gold gradient pill, cream rim, bold white text ----
NW, NH = X(248), Y(62)
grad = np.zeros((NH, NW, 3), np.uint8)
for y in range(NH):
    grad[y, :] = lerp((255, 216, 96), (214, 150, 40), y / (NH - 1))
mask = Image.new("L", (NW, NH), 0)
ImageDraw.Draw(mask).rounded_rectangle([X(3), X(3), NW - X(4), NH - X(4)], X(26), fill=255)
pill = Image.composite(Image.fromarray(grad, "RGB").convert("RGBA"),
                       Image.new("RGBA", (NW, NH), (0, 0, 0, 0)), mask)
pd = ImageDraw.Draw(pill)
pd.rounded_rectangle([X(3), X(3), NW - X(4), NH - X(4)], X(26), outline=(255, 246, 214), width=LW(3))
fnb = ImageFont.truetype(ROUND, PT(30))
bb = pd.textbbox((0, 0), "NEW BEST!", font=fnb)
tx = (NW - (bb[2] - bb[0])) / 2 - bb[0]; ty = (NH - (bb[3] - bb[1])) / 2 - bb[1]
pd.text((tx, ty + Y(2)), "NEW BEST!", font=fnb, fill=(150, 92, 18))
pd.text((tx, ty), "NEW BEST!", font=fnb, fill=(255, 255, 255), stroke_width=LW(1), stroke_fill=(160, 96, 20))
nbr = pill.rotate(-6, expand=True, resample=Image.BICUBIC)


# ---- emit both sprites into the firmware ----
def rgb565(im):
    a = np.array(im.convert("RGB"))
    v = ((a[..., 0] >> 3).astype(np.uint16) << 11) | ((a[..., 1] >> 2).astype(np.uint16) << 5) | (a[..., 2] >> 3)
    return np.dstack([(v & 0xFF).astype(np.uint8), (v >> 8).astype(np.uint8)]).reshape(-1), "LV_COLOR_FORMAT_RGB565", im.width * 2


def rgb565a8(im):
    a = np.array(im.convert("RGBA"))
    v = ((a[..., 0] >> 3).astype(np.uint16) << 11) | ((a[..., 1] >> 2).astype(np.uint16) << 5) | (a[..., 2] >> 3)
    px = np.dstack([(v & 0xFF).astype(np.uint8), (v >> 8).astype(np.uint8)]).reshape(-1)
    return np.concatenate([px, a[..., 3].astype(np.uint8).reshape(-1)]), "LV_COLOR_FORMAT_RGB565A8", im.width * 2


# The narrow file shares the wide header, so it says so and never writes its own.
with open(OUT + "/gameover_img%s.c" % SUFFIX, "w") as f:
    f.write('#include "lvgl.h"\n')
    if SUFFIX:
        f.write('#include "gameover_img.h"\n')
    f.write('\n')
    for nm, im in [("gameover", base), ("newbest", nbr)]:
        data, cf, stride = (rgb565(im) if nm == "gameover" else rgb565a8(im))
        f.write("static const uint8_t %s_map[] = {%s};\n" % (nm, ",".join(map(str, data.tolist()))))
        f.write("const lv_image_dsc_t img_%s = {\n" % nm)
        f.write("  .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = %s,\n" % cf)
        f.write("             .flags = 0, .w = %d, .h = %d, .stride = %d },\n" % (im.width, im.height, stride))
        f.write("  .data_size = sizeof(%s_map), .data = %s_map,\n};\n\n" % (nm, nm))
if not SUFFIX:
    with open(OUT + "/gameover_img.h", "w") as f:
        f.write('#pragma once\n#include "lvgl.h"\nextern const lv_image_dsc_t img_gameover;\nextern const lv_image_dsc_t img_newbest;\n')
print("wrote gameover_img%s.c (%dx%d + newbest %dx%d)" % (SUFFIX, W, H, nbr.width, nbr.height))

# ---- preview: base + sample numbers + newbest (positions mirror show_game_over in main.c) ----
prev = base.convert("RGBA"); pd = ImageDraw.Draw(prev)
fnum = ImageFont.truetype(ROUND, PT(58))
bb = pd.textbbox((0, 0), "47", font=fnum)
if NARROW:   # glyph tops where main.c's OVER_* rows put them
    fnum = ImageFont.truetype(ROUND, 40)
    bb = pd.textbbox((0, 0), "47", font=fnum)
    pd.text(((W - (bb[2] - bb[0])) / 2, 153 - bb[1]), "47", font=fnum, fill=(255, 255, 255))
else:
    pd.text(((W - (bb[2] - bb[0])) / 2, y0 + Y(38)), "47", font=fnum, fill=(255, 255, 255))
fbest = ImageFont.truetype(ROUND, PT(28))
bb = pd.textbbox((0, 0), "BEST  58", font=fbest)
if NARROW:
    pd.text(((W - (bb[2] - bb[0])) / 2, 195 - bb[1]), "BEST  58", font=fbest, fill=(236, 200, 120))
else:
    pd.text(((W - (bb[2] - bb[0])) / 2, y0 + Y(112)), "BEST  58", font=fbest, fill=(236, 200, 120))
prev.alpha_composite(nbr, ((W - nbr.width) // 2, 60 if NARROW else Y(128)))
prev.convert("RGB").save("/tmp/gameover_mock%s.png" % SUFFIX)
print("saved preview /tmp/gameover_mock%s.png" % SUFFIX)
