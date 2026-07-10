#!/usr/bin/env python3
"""GAME OVER screen for FRUIT ISLAND (LANDSCAPE 800x480). Emits two LVGL sprites:
  img_gameover  (800x480 RGB565)  - synthwave backdrop, logo, card frame, PLAY AGAIN, fruit.
  img_newbest   (RGB565A8)        - the rotated NEW BEST! ribbon (shown when beaten).
Dynamic score/best numbers are overlaid as LVGL labels; card/label coords stay in sync with
show_game_over() in main.c (over_lbl y=240, best y=316, newbest y=128)."""
import os
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scene import synthwave_scene, fill_holes

W, H = 800, 480
CX = W // 2
ROUND = "/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EMO = os.path.join(ROOT, "assets/emoji")
OUT = os.path.join(ROOT, "main")
HZ = 318


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


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
img = img.filter(ImageFilter.GaussianBlur(5))
img = Image.alpha_composite(img, Image.new("RGBA", (W, H), (6, 10, 26, 135)))
vg = (np.clip((np.sqrt((xx - CX) ** 2 + (yy - 240) ** 2) / 480 - 0.4) / 0.6, 0, 1) * 175).astype(np.uint8)
vv = np.zeros((H, W, 4), np.uint8); vv[..., 3] = vg
img = Image.alpha_composite(img, Image.fromarray(vv, "RGBA"))


def logo_line(txt, size, cy, top, bot):
    PAD = 200
    f = ImageFont.truetype(ROUND, size)
    tmp = Image.new("RGBA", (W, PAD), (0, 0, 0, 0)); td = ImageDraw.Draw(tmp)
    bb = td.textbbox((0, 0), txt, font=f, stroke_width=9)
    tw, th = bb[2] - bb[0], bb[3] - bb[1]
    x = (W - tw) / 2 - bb[0]; y = 28
    # cream backing = the stroked text itself (original look); fill_holes only
    # plugs enclosed apertures (A's triangle) so no shadow fragment shows through
    plate = Image.new("L", (W, PAD), 0)
    ImageDraw.Draw(plate).text((x, y), txt, font=f, fill=255, stroke_width=9, stroke_fill=255)
    plate = fill_holes(plate)
    sh = Image.new("RGBA", (W, PAD), (0, 0, 0, 0))
    sh.paste((0, 0, 0, 160), (0, 7), plate)
    tmp.alpha_composite(sh.filter(ImageFilter.GaussianBlur(4)))
    tmp.paste((252, 244, 220, 255), (0, 0), plate)
    mask = Image.new("L", (W, PAD), 0); ImageDraw.Draw(mask).text((x, y), txt, font=f, fill=255)
    grad = np.zeros((PAD, W, 3), np.uint8)
    for yy3 in range(PAD):
        grad[yy3, :] = lerp(top, bot, np.clip((yy3 - y) / th, 0, 1))
    tmp.paste(Image.fromarray(grad, "RGB").convert("RGBA"), (0, 0), mask)
    img.alpha_composite(tmp, (0, int(cy - 100)))


logo_line("GAME OVER", 66, 82, (255, 209, 96), (231, 116, 44))  # single line: fits landscape, no top clip

# ---- score card (frame + SCORE label + divider; numbers overlaid at runtime) ----
card = Image.new("RGBA", (W, H), (0, 0, 0, 0)); cd = ImageDraw.Draw(card)
x0, y0, x1, y1 = CX - 165, 202, CX + 165, 352
sh = Image.new("RGBA", (W, H), (0, 0, 0, 0))
ImageDraw.Draw(sh).rounded_rectangle([x0, y0 + 10, x1, y1 + 10], 28, fill=(0, 0, 0, 130))
card = Image.alpha_composite(card, sh.filter(ImageFilter.GaussianBlur(7))); cd = ImageDraw.Draw(card)
cd.rounded_rectangle([x0, y0, x1, y1], 28, fill=(30, 22, 18, 235), outline=(236, 184, 84), width=4)
fs = ImageFont.truetype(ROUND, 28)
bb = cd.textbbox((0, 0), "SCORE", font=fs, stroke_width=2)
cd.text(((W - (bb[2] - bb[0])) / 2, y0 + 14), "SCORE", font=fs, fill=(236, 200, 120), stroke_width=2, stroke_fill=(0, 0, 0))
cd.line([x0 + 44, y0 + 104, x1 - 44, y0 + 104], fill=(236, 184, 84, 140), width=2)
img.alpha_composite(card)

# ---- PLAY AGAIN button ----
btn = Image.new("RGBA", (W, H), (0, 0, 0, 0))
bx0, by0, bx1, by1 = CX - 168, 372, CX + 168, 444
sha = Image.new("RGBA", (W, H), (0, 0, 0, 0))
ImageDraw.Draw(sha).rounded_rectangle([bx0, by0 + 8, bx1, by1 + 8], 38, fill=(0, 0, 0, 130))
btn = Image.alpha_composite(btn, sha.filter(ImageFilter.GaussianBlur(6))); bd = ImageDraw.Draw(btn)
bd.rounded_rectangle([bx0, by0, bx1, by1], 38, fill=(58, 178, 78), outline=(252, 230, 150), width=4)
fb = ImageFont.truetype(ROUND, 40)
tb = bd.textbbox((0, 0), "PLAY AGAIN", font=fb)
bd.text(((W - (tb[2] - tb[0])) / 2, by0 + 14), "PLAY AGAIN", font=fb, fill=(255, 255, 255),
        stroke_width=2, stroke_fill=(30, 110, 50))
img.alpha_composite(btn)

# ---- fruit accents flanking the title (cherries nudged clear of the MENU pill) ----
for nm, pos, sz in [("watermelon", (150, 56), 90), ("cherries", (545, 96), 88)]:
    fr = Image.open(f"{EMO}/{nm}.png").convert("RGBA"); fr.thumbnail((sz, sz), Image.LANCZOS)
    img.alpha_composite(fr, pos)

# ---- MENU button (top-right, card style; inset for panel overscan) ----
# hit region lives in main.c game_tick ST_OVER tap handling -- keep in sync
mx0, my0, mx1, my1 = 612, 30, 752, 84
mb = Image.new("RGBA", (W, H), (0, 0, 0, 0))
msh = Image.new("RGBA", (W, H), (0, 0, 0, 0))
ImageDraw.Draw(msh).rounded_rectangle([mx0, my0 + 6, mx1, my1 + 6], 27, fill=(0, 0, 0, 120))
mb = Image.alpha_composite(mb, msh.filter(ImageFilter.GaussianBlur(5))); md = ImageDraw.Draw(mb)
md.rounded_rectangle([mx0, my0, mx1, my1], 27, fill=(30, 22, 18, 235), outline=(236, 184, 84), width=3)
fm = ImageFont.truetype(ROUND, 26)
tb = md.textbbox((0, 0), "MENU", font=fm)
md.text(((mx0 + mx1 - (tb[2] - tb[0])) / 2 - tb[0], (my0 + my1 - (tb[3] - tb[1])) / 2 - tb[1]),
        "MENU", font=fm, fill=(236, 200, 120))
img.alpha_composite(mb)

base = img.convert("RGB")  # firmware image (no dynamic numbers, no newbest)

# ---- NEW BEST! ribbon: clean gold gradient pill, cream rim, bold white text ----
NW, NH = 248, 62
grad = np.zeros((NH, NW, 3), np.uint8)
for y in range(NH):
    grad[y, :] = lerp((255, 216, 96), (214, 150, 40), y / (NH - 1))
mask = Image.new("L", (NW, NH), 0)
ImageDraw.Draw(mask).rounded_rectangle([3, 3, NW - 4, NH - 4], 26, fill=255)
pill = Image.composite(Image.fromarray(grad, "RGB").convert("RGBA"),
                       Image.new("RGBA", (NW, NH), (0, 0, 0, 0)), mask)
pd = ImageDraw.Draw(pill)
pd.rounded_rectangle([3, 3, NW - 4, NH - 4], 26, outline=(255, 246, 214), width=3)
fnb = ImageFont.truetype(ROUND, 30)
bb = pd.textbbox((0, 0), "NEW BEST!", font=fnb)
tx = (NW - (bb[2] - bb[0])) / 2 - bb[0]; ty = (NH - (bb[3] - bb[1])) / 2 - bb[1]
pd.text((tx, ty + 2), "NEW BEST!", font=fnb, fill=(150, 92, 18))
pd.text((tx, ty), "NEW BEST!", font=fnb, fill=(255, 255, 255), stroke_width=1, stroke_fill=(160, 96, 20))
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


with open(OUT + "/gameover_img.c", "w") as f:
    f.write('#include "lvgl.h"\n\n')
    for nm, im in [("gameover", base), ("newbest", nbr)]:
        data, cf, stride = (rgb565(im) if nm == "gameover" else rgb565a8(im))
        f.write("static const uint8_t %s_map[] = {%s};\n" % (nm, ",".join(map(str, data.tolist()))))
        f.write("const lv_image_dsc_t img_%s = {\n" % nm)
        f.write("  .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = %s,\n" % cf)
        f.write("             .flags = 0, .w = %d, .h = %d, .stride = %d },\n" % (im.width, im.height, stride))
        f.write("  .data_size = sizeof(%s_map), .data = %s_map,\n};\n\n" % (nm, nm))
with open(OUT + "/gameover_img.h", "w") as f:
    f.write('#pragma once\n#include "lvgl.h"\nextern const lv_image_dsc_t img_gameover;\nextern const lv_image_dsc_t img_newbest;\n')
print("wrote gameover_img.c (%dx%d + newbest %dx%d)" % (W, H, nbr.width, nbr.height))

# ---- preview: base + sample numbers + newbest (positions mirror show_game_over in main.c) ----
prev = base.convert("RGBA"); pd = ImageDraw.Draw(prev)
fnum = ImageFont.truetype(ROUND, 58)
bb = pd.textbbox((0, 0), "47", font=fnum)
pd.text(((W - (bb[2] - bb[0])) / 2, y0 + 38), "47", font=fnum, fill=(255, 255, 255))
fbest = ImageFont.truetype(ROUND, 28)
bb = pd.textbbox((0, 0), "BEST  58", font=fbest)
pd.text(((W - (bb[2] - bb[0])) / 2, y0 + 112), "BEST  58", font=fbest, fill=(236, 200, 120))
prev.alpha_composite(nbr, ((W - nbr.width) // 2, 128))
prev.convert("RGB").save("/tmp/gameover_mock.png")
print("saved preview /tmp/gameover_mock.png")
