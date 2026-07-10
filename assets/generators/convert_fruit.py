#!/usr/bin/env python3
"""LVGL sprites from the CC0 'Sprites Fruits' pack (OpenGameArt, public domain)
+ procedural apple/pineapple cross-sections, bomb, explosion, juice splat,
droplet, hearts. Replaces the cartoon emoji fruit with realistic art."""
import os
import numpy as np
from PIL import Image, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PACK = os.path.join(ROOT, "assets/fruit-pack/Items")
EMO = os.path.join(ROOT, "assets/emoji")
OUT = os.path.join(ROOT, "main")
os.makedirs(OUT, exist_ok=True)
c = ['#include "lvgl.h"', ""]
h = ['#pragma once', '#include "lvgl.h"', "", "#ifdef __cplusplus", 'extern "C" {', "#endif", ""]


def emit(name, im):
    a = np.array(im.convert("RGBA")); w, hh = im.size
    r = a[..., 0].astype(np.uint16); g = a[..., 1].astype(np.uint16); b = a[..., 2].astype(np.uint16)
    al = a[..., 3].astype(np.uint8)
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    lo = (v & 0xFF).astype(np.uint8); hi = ((v >> 8) & 0xFF).astype(np.uint8)
    data = np.concatenate([np.dstack([lo, hi]).reshape(-1), al.reshape(-1)])
    c.append(f"static const uint8_t {name}_map[]={{{','.join(str(int(x)) for x in data)}}};")
    c.append(f"const lv_image_dsc_t img_{name}={{")
    c.append(f"  .header={{.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_RGB565A8,.flags=0,.w={w},.h={hh},.stride={w*2}}},")
    c.append(f"  .data_size=sizeof({name}_map),.data={name}_map,}};\n")
    h.append(f"extern const lv_image_dsc_t img_{name};")


def fit(im, S, pad=0.96):
    im = im.convert("RGBA")
    bb = im.getbbox()
    if bb:
        im = im.crop(bb)
    # de-fringe: erode the alpha to cut the colored halo left by sloppy cutouts
    r, g, b, a = im.split()
    a = a.filter(ImageFilter.MinFilter(5)).filter(ImageFilter.GaussianBlur(0.6))
    im = Image.merge("RGBA", (r, g, b, a))
    im.thumbnail((int(S * pad), int(S * pad)), Image.LANCZOS)
    cv = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    cv.alpha_composite(im, ((S - im.width) // 2, (S - im.height) // 2))
    return cv


def round_melon(S):
    cx = cy = (S - 1) / 2.0; r = S * 0.47
    yy, xx = np.mgrid[0:S, 0:S].astype(float)
    dx = (xx - cx) / r; dy = (yy - cy) / r; d2 = dx * dx + dy * dy; z = np.sqrt(np.clip(1 - d2, 0, 1))
    lx, ly = -0.42, -0.5; lz = np.sqrt(max(1 - lx * lx - ly * ly, 0))
    ndl = np.clip(dx * lx + dy * ly + z * lz, 0, 1); diffuse = 0.58 + 0.42 * ndl
    rgb = np.array([58, 152, 73], float)[None, None, :] * diffuse[..., None]
    lon = np.arctan2(dx, z + 1e-6); wig = 0.45 * np.sin(dy * 7.0) + 0.18 * np.sin(dy * 13.0)
    band = np.clip((np.cos(lon * 9.0 + wig) - 0.05) / 0.22, 0, 1)
    rgb = rgb * (1 - 0.9 * band[..., None]) + np.array([24, 86, 46], float)[None, None, :] * diffuse[..., None] * (0.9 * band[..., None])
    rgb *= (1 - 0.18 * np.clip((d2 - 0.5) / 0.5, 0, 1))[..., None]; rgb += 16.0 * (ndl ** 10)[..., None]
    rgb = np.clip(rgb, 0, 255).astype(np.uint8); alpha = (np.clip((1.0 - d2) / 0.05, 0, 1) * 255).astype(np.uint8)
    return Image.fromarray(np.dstack([rgb, alpha]), "RGBA")


def load(n, S):
    return fit(Image.open(f"{PACK}/{n:02d}.png"), S)


def split_halves(name, sq):
    W = sq.width; a = np.array(sq)
    L = a.copy(); L[:, W // 2:, 3] = 0
    R = a.copy(); R[:, :W // 2, 3] = 0
    emit(name + "_half", Image.fromarray(L, "RGBA"))
    emit(name + "_halfr", Image.fromarray(R, "RGBA"))


def mirror_halves(name, sq):
    emit(name + "_half", sq)
    emit(name + "_halfr", sq.transpose(Image.FLIP_LEFT_RIGHT))


def _polar(S):
    yy, xx = np.mgrid[0:S, 0:S].astype(float)
    cx = cy = (S - 1) / 2.0; dx = xx - cx; dy = yy - cy
    R = S * 0.47; rn = np.sqrt(dx * dx + dy * dy) / R; ang = np.arctan2(dy, dx)
    return xx, yy, cx, cy, rn, ang, R


def _disc(rgb, rn):
    al = (np.clip((1.0 - rn) / 0.06, 0, 1) * 255).astype(np.uint8)
    return Image.fromarray(np.dstack([np.clip(rgb, 0, 255).astype(np.uint8), al]), "RGBA")


def _seeds(rgb, xx, yy, cx, cy, R, n, rad, color, seed_rn=0.32):
    for k in range(n):
        th = 2 * np.pi * k / n - np.pi / 2; ux, uy = np.cos(th), np.sin(th)
        sx = cx + ux * R * seed_rn; sy = cy + uy * R * seed_rn
        er = (xx - sx) * ux + (yy - sy) * uy; et = -(xx - sx) * uy + (yy - sy) * ux
        m = (er * er) / (R * rad * 1.7) ** 2 + (et * et) / (R * rad) ** 2 < 1
        rgb[m] = color


def sec_apple(S, rind=(228, 58, 56), flesh=(250, 244, 214)):
    xx, yy, cx, cy, rn, ang, R = _polar(S)
    rgb = np.array(flesh, float)[None, None, :] * (1.02 - 0.08 * rn)[..., None]
    rgb[(rn > 0.86) & (rn <= 0.90)] = (253, 248, 232); rgb[rn > 0.90] = rind
    _seeds(rgb, xx, yy, cx, cy, R, 5, 0.05, (96, 60, 36)); rgb[rn < 0.06] = (238, 226, 190)
    return _disc(rgb, rn)


def sec_watermelon(S):
    xx, yy, cx, cy, rn, ang, R = _polar(S)
    rgb = np.array([236, 66, 80], float)[None, None, :] * (1.05 - 0.11 * rn)[..., None]  # red flesh, lit center
    rgb[rn > 0.84] = (250, 250, 240)     # thin cream line
    rgb[rn > 0.865] = (132, 206, 122)    # light green inner rind
    rgb[rn > 0.93] = (44, 130, 60)       # dark green outer rind
    _seeds(rgb, xx, yy, cx, cy, R, 8, 0.045, (38, 30, 30), seed_rn=0.52)   # outer seed ring
    _seeds(rgb, xx, yy, cx, cy, R, 5, 0.04, (38, 30, 30), seed_rn=0.27)    # inner seeds
    return _disc(rgb, rn)


def sec_orange(S):
    xx, yy, cx, cy, rn, ang, R = _polar(S)
    rgb = np.array([255, 158, 36], float)[None, None, :] * (1.06 - 0.12 * rn)[..., None]  # citrus flesh
    seg = np.abs(((ang / np.pi * 5) % 1.0) - 0.5)        # 10 wedges
    rgb[seg > 0.45] = (255, 230, 194)                    # white segment walls
    rgb[rn > 0.88] = (255, 226, 188)                     # inner pith
    rgb[rn > 0.93] = (255, 150, 40)                      # peel
    rgb[rn < 0.07] = (255, 224, 184)                     # center pith
    return _disc(rgb, rn)


def sec_pine(S):
    xx, yy, cx, cy, rn, ang, R = _polar(S)
    rgb = np.array([255, 212, 74], float)[None, None, :] * (1.06 - 0.10 * rn)[..., None]  # brighter flesh
    rgb *= (0.94 + 0.06 * (0.5 + 0.5 * np.sin(ang * 30)))[..., None]   # softer fibers
    rgb *= (0.96 + 0.04 * (0.5 + 0.5 * np.sin(rn * 24)))[..., None]    # softer growth rings
    eye = (np.sin(ang * 18) > 0.55) & (np.sin(rn * 20) > 0.45)
    rgb[eye] = rgb[eye] * 0.86 + np.array([214, 168, 52]) * 0.14
    rgb[rn < 0.18] = (252, 236, 168); rgb[(rn >= 0.18) & (rn < 0.215)] = (236, 206, 110)
    ri = 0.85 + 0.04 * np.sin(ang * 16); rgb[rn > ri] = (210, 176, 80); rgb[rn > ri + 0.07] = (150, 120, 48)
    return _disc(rgb, rn)


def make_splat(S):
    yy, xx = np.mgrid[0:S, 0:S].astype(float); cx = cy = (S - 1) / 2.0; dx = xx - cx; dy = yy - cy
    ang = np.arctan2(dy, dx); r = np.sqrt(dx * dx + dy * dy) / (S * 0.5)
    core = 0.32 + 0.06 * np.sin(ang * 5 + 0.3) + 0.04 * np.sin(ang * 8 + 1.0)
    a = np.clip((core - r) / 0.045, 0, 1); rng = np.random.default_rng(5)
    for _ in range(10):
        th = rng.uniform(0, 2 * np.pi); ln = rng.uniform(0.55, 0.96); hf = rng.uniform(0.05, 0.12)
        dth = (ang - th + np.pi) % (2 * np.pi) - np.pi; prof = np.clip(1 - np.abs(dth) / hf, 0, 1)
        tip = core + (ln - core) * prof; a = np.maximum(a, np.clip((tip - r) / 0.05, 0, 1) * np.clip(prof * 1.6, 0, 1))
        tx = cx + np.cos(th) * ln * (S * 0.5); ty = cy + np.sin(th) * ln * (S * 0.5)
        dd = np.sqrt((xx - tx) ** 2 + (yy - ty) ** 2); a = np.maximum(a, np.clip((rng.uniform(2.0, 4.5) - dd) / 1.5, 0, 1))
    al = (np.clip(a, 0, 1) * 255).astype(np.uint8)
    return Image.fromarray(np.dstack([np.full((S, S, 3), 255, np.uint8), al]), "RGBA")


def heart(S, color):
    yy, xx = np.mgrid[0:S, 0:S].astype(float); m = S * 0.15; scale = (S - 2 * m) / 2.4
    cx = (S - 1) / 2.0; cy = m + scale; x = (xx - cx) / scale; y = (cy - yy) / scale
    f = (x * x + y * y - 1) ** 3 - x * x * (y ** 3); al = np.where(f <= 0, 255, 0).astype(np.uint8)
    sh = 0.68 + 0.32 * np.clip(y + 0.2, 0, 1)
    rgb = np.clip(np.array(color, float)[None, None, :] * sh[..., None], 0, 255).astype(np.uint8)
    return Image.fromarray(np.dstack([rgb, al]), "RGBA")


# ONE consistent set: every whole fruit is from the emoji art; cut faces are procedural
# cartoon cross-sections (so sliceable fruit reveal flesh, matching the whole-fruit style).
def emo(name, S):
    return fit(Image.open(f"{EMO}/{name}.png"), S)

# name, emoji file, cross-section fn (None = burst, no slice), size
ROSTER = [
    ("watermelon", "watermelon", sec_watermelon, 104),
    ("apple",      "red_apple",  sec_apple,       94),
    ("orange",     "tangerine",  sec_orange,      94),
    ("pineapple",  "pineapple",  sec_pine,       112),  # noticeably bigger
    ("strawberry", "strawberry", None,           100),
    ("cherries",   "cherries",   None,            90),
    ("grapes",     "grapes",     None,            94),
]
# watermelon's whole is a procedural green melon (emoji 🍉 is a slice); it slices to red flesh
def whole_img(name, efile, S):
    return round_melon(S) if name == "watermelon" else emo(efile, S)

for name, efile, sec, S in ROSTER:
    emit(name, whole_img(name, efile, S))
    if sec is not None:
        split_halves(name, sec(S))
    print("fruit", name, S)

emit("bomb", fit(Image.open(f"{EMO}/bomb.png"), 92))
emit("explosion", fit(Image.open(f"{EMO}/collision.png"), 144))
emit("splat", make_splat(110))
_S = 16; _yy, _xx = np.mgrid[0:_S, 0:_S].astype(float)
_d = np.sqrt((_xx - (_S - 1) / 2) ** 2 + (_yy - (_S - 1) / 2) ** 2) / (_S / 2)
_al = (np.clip((0.94 - _d) / 0.14, 0, 1) * 255).astype(np.uint8)
emit("droplet", Image.fromarray(np.dstack([np.full((_S, _S, 3), 255, np.uint8), _al]), "RGBA"))
emit("heart", heart(40, [232, 60, 60]))
emit("heart_empty", heart(40, [86, 56, 60]))

h += ["", "#ifdef __cplusplus", "}", "#endif", ""]
open(f"{OUT}/sprites.c", "w").write("\n".join(c))
open(f"{OUT}/sprites.h", "w").write("\n".join(h))
print("bytes:", os.path.getsize(f"{OUT}/sprites.c"))

# verification sheet: whole fruit (top) + its cross-section (bottom), drawn at true relative sizes
sheet = Image.new("RGBA", (len(ROSTER) * 122, 2 * 122 + 16), (18, 22, 28, 255))
for i, (name, efile, sec, S) in enumerate(ROSTER):
    sheet.alpha_composite(whole_img(name, efile, S), (i * 122 + 8, 8 + (120 - S) // 2))
    if sec is not None:
        sheet.alpha_composite(sec(S), (i * 122 + 8, 130 + (120 - S) // 2))
sheet.convert("RGB").save("/tmp/newfruit_sheet.png")
print("sheet saved")
