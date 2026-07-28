#!/usr/bin/env python3
"""Baked FRUIT ISLAND menu: synthwave/outrun sunset-island hero + logo + glossy button.
LANDSCAPE 800x480 (rendered, then rotated into the panel by rot_flush in main.c).

The logo LETTERS are emitted as individual RGB565A8 sprites (main/menu_logo.c) so the
firmware can drop them in one-by-one on menu entry; the backdrop (img_menu) bakes only
their drop SHADOWS, and the letters land exactly on them. The screensaver backdrop
(img_saver) keeps the full logo baked -- it never animates there."""
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
HZ = 318
PAD = 260   # tall scratch canvas per logo line


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


# ---- sky gradient (sunset), vertical ----
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


# ---- logo: "FRUIT ISLAND" stacked, centered, upper area ----
def line_layout(txt, size):
    f = ImageFont.truetype(ROUND, size)
    d = ImageDraw.Draw(Image.new("L", (1, 1)))
    bb = d.textbbox((0, 0), txt, font=f, stroke_width=9)
    return f, (W - (bb[2] - bb[0])) / 2 - bb[0], bb[3] - bb[1]


def grad_img(top, bot, th, w):
    g = np.zeros((PAD, w, 3), np.uint8)
    for y3 in range(PAD):
        g[y3, :] = lerp(top, bot, np.clip((y3 - 36) / th, 0, 1))
    return Image.fromarray(g, "RGB").convert("RGBA")


def draw_logo(dst, txt, size, cy, top, bot, shadow_only=False):
    f, x, th = line_layout(txt, size)
    y = 36
    # cream backing = the stroked text itself (original look), with only fully-
    # enclosed holes filled so no shadow fragment floats in the A's aperture
    plate = Image.new("L", (W, PAD), 0)
    ImageDraw.Draw(plate).text((x, y), txt, font=f, fill=255, stroke_width=9, stroke_fill=255)
    plate = fill_holes(plate)
    tmp = Image.new("RGBA", (W, PAD), (0, 0, 0, 0))
    sh = Image.new("RGBA", (W, PAD), (0, 0, 0, 0))
    sh.paste((0, 0, 0, 160), (0, 7), plate)
    tmp.alpha_composite(sh.filter(ImageFilter.GaussianBlur(4)))
    if not shadow_only:
        tmp.paste((252, 244, 220, 255), (0, 0), plate)
        mask = Image.new("L", (W, PAD), 0)
        ImageDraw.Draw(mask).text((x, y), txt, font=f, fill=255)
        tmp.paste(grad_img(top, bot, th, W), (0, 0), mask)
    dst.alpha_composite(tmp, (0, int(cy - 130)))


def letter_sprites(txt, size, cy, top, bot):
    """Each letter as its own sprite: shadow + cream plate + gradient, all rendered in
    full-line coordinates. The shadow travels WITH the falling letter; it is erased
    wherever a NEIGHBOR's plate sits at rest (those pixels are covered by the
    neighbor's cream anyway), so the settled seams show no double-darkening.
    Returns [(rgba_sprite, screen_x, screen_y), ...]."""
    f, x, th = line_layout(txt, size)
    y = 36
    d = ImageDraw.Draw(Image.new("L", (1, 1)))
    plates = []
    for i, ch in enumerate(txt):
        adv = d.textlength(txt[:i], font=f)
        p = Image.new("L", (W, PAD), 0)
        ImageDraw.Draw(p).text((x + adv, y), ch, font=f, fill=255, stroke_width=9, stroke_fill=255)
        plates.append(fill_holes(p))
    out = []
    for i, ch in enumerate(txt):
        adv = d.textlength(txt[:i], font=f)
        sh = Image.new("RGBA", (W, PAD), (0, 0, 0, 0))
        sh.paste((0, 0, 0, 160), (0, 7), plates[i])
        sh = sh.filter(ImageFilter.GaussianBlur(4))
        others = np.zeros((PAD, W), bool)
        for j, pj in enumerate(plates):
            if j != i:
                others |= np.array(pj) > 128
        sa = np.array(sh)
        sa[..., 3] = np.where(others, 0, sa[..., 3])
        c = Image.fromarray(sa, "RGBA")
        c.paste((252, 244, 220, 255), (0, 0), plates[i])
        mask = Image.new("L", (W, PAD), 0)
        ImageDraw.Draw(mask).text((x + adv, y), ch, font=f, fill=255)
        c.paste(grad_img(top, bot, th, W), (0, 0), mask)
        bx = c.getbbox()
        bx = (bx[0] - 1, bx[1] - 1, bx[2] + 1, bx[3] + 1)
        out.append((c.crop(bx), bx[0], int(cy - 130) + bx[1]))
    return out


L1 = ("FRUIT", 92, 86, (255, 196, 64), (244, 96, 40))
L2 = ("ISLAND", 82, 166, (255, 196, 64), (244, 96, 40))

# ---- screensaver backdrop: scene + FULL logo, no UI chrome ----
logo_full = img.copy()
draw_logo(logo_full, *L1)
draw_logo(logo_full, *L2)
svg = (np.clip((np.sqrt((xx - CX) ** 2 + (yy - 240) ** 2) / 480 - 0.40) / 0.5, 0, 1) * 150).astype(np.uint8)
saver = Image.alpha_composite(logo_full, Image.fromarray(np.dstack([np.zeros((H, W, 3), np.uint8), svg]), "RGBA"))
saver.convert("RGB").save("/tmp/saver_mock.png")

# ---- menu backdrop: NO logo at all (the live letter sprites carry their own shadows;
# a naked baked shadow would sit like a dark smear until the drop-in finishes) ----
letters = letter_sprites(*L1) + letter_sprites(*L2)

# ---- fruit accents flanking the logo: live sprites too (hop in after the letters,
# then float gently), so the backdrop stays bare here ----
fruits = []
for nm, pos, sz in [("cherries", (150, 64), 92), ("grapes", (560, 60), 90),
                    ("watermelon", (172, 176), 80), ("strawberry", (566, 184), 76)]:
    fr = Image.open(f"{EMO}/{nm}.png").convert("RGBA")
    fr.thumbnail((sz, sz), Image.LANCZOS)
    fruits.append((fr, pos[0], pos[1]))

# ---- PLAY button ----
btn = Image.new("RGBA", (W, H), (0, 0, 0, 0)); bd = ImageDraw.Draw(btn)
bx0, by0, bx1, by1 = CX - 150, 356, CX + 150, 428  # lifted so the tagline below clears the 480px bottom edge
sha = Image.new("RGBA", (W, H), (0, 0, 0, 0))
ImageDraw.Draw(sha).rounded_rectangle([bx0, by0 + 8, bx1, by1 + 8], 36, fill=(0, 0, 0, 130))
btn = Image.alpha_composite(btn, sha.filter(ImageFilter.GaussianBlur(6))); bd = ImageDraw.Draw(btn)
bd.rounded_rectangle([bx0, by0, bx1, by1], 36, fill=(58, 178, 78), outline=(252, 230, 150), width=4)
fb = ImageFont.truetype(ROUND, 44)
tb = bd.textbbox((0, 0), "PLAY", font=fb)
bd.text(((W - (tb[2] - tb[0])) / 2, by0 + 12), "PLAY", font=fb, fill=(255, 255, 255),
        stroke_width=2, stroke_fill=(30, 110, 50))
img.alpha_composite(btn)

# ---- tagline (drawn gold dot separator, no missing-glyph tofu) ----
ft = ImageFont.truetype(ROUND, 24)
dd = ImageDraw.Draw(img)
left, right = "slice the fruit", "dodge the bombs"
lw = dd.textbbox((0, 0), left, font=ft)[2]
rw = dd.textbbox((0, 0), right, font=ft)[2]
gap = 32
x0 = (W - (lw + gap + rw)) / 2
dd.text((x0, 436), left, font=ft, fill=(245, 232, 205), stroke_width=2, stroke_fill=(0, 0, 0))
cxd = x0 + lw + gap / 2
dd.ellipse([cxd - 4, 448, cxd + 4, 456], fill=(252, 210, 90))
dd.text((x0 + lw + gap, 436), right, font=ft, fill=(245, 232, 205), stroke_width=2, stroke_fill=(0, 0, 0))

# ---- vignette ----
vg = (np.clip((np.sqrt((xx - CX) ** 2 + (yy - 240) ** 2) / 480 - 0.58) / 0.42, 0, 1) * 130).astype(np.uint8)
vimg = np.zeros((H, W, 4), np.uint8); vimg[..., 3] = vg
img = Image.alpha_composite(img, Image.fromarray(vimg, "RGBA"))

# ---- preview: backdrop + letters + fruit at rest = exactly the settled menu ----
prev = img.copy()
for spr, sx, sy in letters:
    prev.alpha_composite(spr, (sx, sy))
for spr, sx, sy in fruits:
    prev.alpha_composite(spr, (sx, sy))
prev.convert("RGB").save("/tmp/menu_mock.png")
print("saved /tmp/menu_mock.png (+ /tmp/saver_mock.png)")


# ---- emit menu backdrop + screensaver as LVGL RGB565, letters as RGB565A8 ----
def decl(name, im):
    rgb = np.array(im.convert("RGB"))
    r = (rgb[..., 0] >> 3).astype(np.uint16); g = (rgb[..., 1] >> 2).astype(np.uint16); b = (rgb[..., 2] >> 3).astype(np.uint16)
    v = (r << 11) | (g << 5) | b
    data = np.dstack([(v & 0xFF).astype(np.uint8), (v >> 8).astype(np.uint8)]).reshape(-1)
    s = "static const uint8_t %s_map[] = {%s};\n\n" % (name, ",".join(map(str, data.tolist())))
    s += "const lv_image_dsc_t img_%s = {\n" % name
    s += "  .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,\n"
    s += "             .flags = 0, .w = %d, .h = %d, .stride = %d },\n" % (im.width, im.height, im.width * 2)
    s += "  .data_size = sizeof(%s_map), .data = %s_map,\n};\n\n" % (name, name)
    return s


def rgb565a8(im):
    a = np.array(im.convert("RGBA"))
    v = ((a[..., 0] >> 3).astype(np.uint16) << 11) | ((a[..., 1] >> 2).astype(np.uint16) << 5) | (a[..., 2] >> 3)
    px = np.dstack([(v & 0xFF).astype(np.uint8), (v >> 8).astype(np.uint8)]).reshape(-1)
    return np.concatenate([px, a[..., 3].astype(np.uint8).reshape(-1)])


OUT = os.path.join(ROOT, "main")
with open(OUT + "/menu_img.c", "w") as f:
    f.write('#include "lvgl.h"\n\n')
    f.write(decl("menu", img))
    f.write(decl("saver", saver))
with open(OUT + "/menu_img.h", "w") as f:
    f.write('#pragma once\n#include "lvgl.h"\n')
    f.write('extern const lv_image_dsc_t img_menu;\n')
    f.write('extern const lv_image_dsc_t img_saver;\n')
print("wrote main/menu_img.c (img_menu + img_saver)")

def emit_a8_set(f, tag, items):
    for i, (spr, sx, sy) in enumerate(items):
        f.write("static const uint8_t %s%d_map[] = {%s};\n" % (tag, i, ",".join(map(str, rgb565a8(spr).tolist()))))


def emit_a8_arr(f, name, tag, n, items):
    f.write("\nconst lv_image_dsc_t %s[%s] = {\n" % (name, n))
    for i, (spr, sx, sy) in enumerate(items):
        f.write("  { .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565A8,\n")
        f.write("               .flags = 0, .w = %d, .h = %d, .stride = %d },\n" % (spr.width, spr.height, spr.width * 2))
        f.write("    .data_size = sizeof(%s%d_map), .data = %s%d_map },\n" % (tag, i, tag, i))
    f.write("};\n")


with open(OUT + "/menu_logo.c", "w") as f:
    f.write('#include "menu_logo.h"\n\n')
    emit_a8_set(f, "lt", letters)
    emit_a8_set(f, "mf", fruits)
    emit_a8_arr(f, "img_logo_lt", "lt", "LOGO_LT_N", letters)
    emit_a8_arr(f, "img_menu_fruit", "mf", "MENU_FRUIT_N", fruits)
    f.write("const int16_t logo_lt_x[LOGO_LT_N] = {%s};\n" % ",".join(str(sx) for _, sx, _ in letters))
    f.write("const int16_t logo_lt_y[LOGO_LT_N] = {%s};\n" % ",".join(str(sy) for _, _, sy in letters))
    f.write("const int16_t menu_fruit_x[MENU_FRUIT_N] = {%s};\n" % ",".join(str(sx) for _, sx, _ in fruits))
    f.write("const int16_t menu_fruit_y[MENU_FRUIT_N] = {%s};\n" % ",".join(str(sy) for _, _, sy in fruits))
with open(OUT + "/menu_logo.h", "w") as f:
    f.write('#pragma once\n#include "lvgl.h"\n')
    f.write('#define LOGO_LT_N %d\n' % len(letters))
    f.write('#define MENU_FRUIT_N %d\n' % len(fruits))
    f.write('extern const lv_image_dsc_t img_logo_lt[LOGO_LT_N];\n')
    f.write('extern const int16_t logo_lt_x[LOGO_LT_N], logo_lt_y[LOGO_LT_N];\n')
    f.write('extern const lv_image_dsc_t img_menu_fruit[MENU_FRUIT_N];\n')
    f.write('extern const int16_t menu_fruit_x[MENU_FRUIT_N], menu_fruit_y[MENU_FRUIT_N];\n')
print("wrote main/menu_logo.c (%d letter + %d fruit sprites)" % (len(letters), len(fruits)))
