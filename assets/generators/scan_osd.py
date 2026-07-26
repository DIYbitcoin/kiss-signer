#!/usr/bin/env python3
# Bakes the camera on-video UI strips into main/scan_osd.c/.h.
# The live camera path bypasses LVGL entirely, so all scanner/entropy feedback
# is drawn INTO the video framebuffer. Strips are 4-bit-alpha (anti-aliased)
# landscape bitmaps blitted by camera_spike.c with the upright-in-landscape
# transform; each state is a mixed-case title plus a muted one-line subtitle.
# A small glyph atlas ('0'-'9' + "of") renders live part counts in real type.
# Preview: /tmp/scan_ui_mock.png (LOOK at it before flashing — hard rule).
import math
import json
import os
import random
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
FONT_LAT = "/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf"
FONT_CJK = {
    "ja": "/System/Library/Fonts/ヒラギノ角ゴシック W6.ttc",
    "ko": "/System/Library/Fonts/AppleSDGothicNeo.ttc",
    "zh-CN": "/System/Library/Fonts/Hiragino Sans GB.ttc",
}
TITLE = 30
SUB = 19
MAXW = 640      # within the 800-px landscape width minus overscan insets

# Firmware/NVS locale order. Keep in lockstep with tools/gen_i18n.py.
LOCALES = [
    "en", "de", "es-MX", "fr", "it", "ja", "ko", "nl", "pl", "pt-BR",
    "ru", "tr", "vi", "zh-CN", "es-ES", "pt-PT", "nb-NO", "sv-SE",
    "da-DK", "cs-CZ", "hr-HR",
]

# (name, title key, subtitle key). The live camera bypasses LVGL, so these
# translated strings are baked as alpha strips for every runtime locale.
STRIPS = [
    ("OSD_SEARCH",  "C_OSD_SEARCH_T",  "C_OSD_SEARCH_S"),
    ("OSD_SEEN",    "C_OSD_SEEN_T",    None),
    ("OSD_CUTOFF",  "C_OSD_CUTOFF_T", "C_OSD_CUTOFF_S"),
    ("OSD_READ",    "C_OSD_READ_T",    None),
    ("OSD_ENT_LOW", "C_OSD_ENT_LOW_T", "C_OSD_ENT_LOW_S"),
    ("OSD_ENT_OK",  "C_OSD_ENT_OK_T",  "C_OSD_ENT_OK_S"),
    ("OSD_CLOSE",   "C_OSD_CLOSE",     None),
]

GLYPHS = [str(d) for d in range(10)] + ["of"]


def locale_font(stem):
    return FONT_CJK.get(stem, FONT_LAT)


def fitted_font(path, text, start, floor):
    for size in range(start, floor - 1, -1):
        f = ImageFont.truetype(path, size)
        if not text or f.getlength(text) <= MAXW - 8:
            return f, size
    return ImageFont.truetype(path, floor), floor


def render_strip(title, sub, font_path=FONT_LAT):
    """Anti-aliased white-on-transparent two-line strip (L mode = alpha)."""
    tf, ts = fitted_font(font_path, title, TITLE, 18)
    sf, ss = fitted_font(font_path, sub, SUB, 12)
    tw = int(tf.getlength(title)) if title else 0
    sw = int(sf.getlength(sub)) if sub else 0
    w = min(MAXW, max(tw, sw) + 8)
    h = (ts + 8) + (ss + 8 if sub else 0)
    im = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(im)
    if title:
        d.text(((w - tw) // 2, 0), title, font=tf, fill=255)
    if sub:
        d.text(((w - sw) // 2, ts + 8), sub, font=sf, fill=145)  # muted
    return im


def render_close(text, font_path=FONT_LAT):
    """The tap-here-to-close hint for the top-left corner."""
    f, _ = fitted_font(font_path, text, 20, 12)
    tw = int(f.getlength(text))
    im = Image.new("L", (tw + 6, 28), 0)
    ImageDraw.Draw(im).text((3, 0), text, font=f, fill=190)
    return im


def render_glyph(s, font_path=FONT_LAT):
    f = ImageFont.truetype(font_path, TITLE)
    tw = int(f.getlength(s))
    im = Image.new("L", (tw + 2, TITLE + 8), 0)
    ImageDraw.Draw(im).text((1, 0), s, font=f, fill=255)
    return im


def pack_a4(im):
    """Pack 8-bit alpha to 4-bit, two pixels per byte, high nibble first."""
    w, h = im.size
    px = im.load()
    out = bytearray((w * h + 1) // 2)
    for y in range(h):
        for x in range(w):
            i = y * w + x
            a = px[x, y] >> 4
            if i & 1:
                out[i >> 1] |= a
            else:
                out[i >> 1] |= a << 4
    return w, h, bytes(out)


def emit(images, names, ctype, cname):
    lines = []
    metas = []
    for (name, im) in zip(names, images):
        w, h, bits = pack_a4(im)
        arr = ", ".join(str(b) for b in bits)
        lines.append(f"static const uint8_t {name}_a4[] = {{{arr}}};")
        metas.append((name, w, h))
    lines.append("")
    lines.append(f"const {ctype} {cname}[] = {{")
    for name, w, h in metas:
        lines.append(f"    {{{w}, {h}, {name}_a4}},")
    lines.append("};")
    return lines


# ---- bake ----
locale_strips = {}
locale_of = {}
for stem in LOCALES:
    with open(ROOT / "i18n" / f"{stem}.json", encoding="utf-8") as f:
        strings = json.load(f)
    font_path = locale_font(stem)
    images = []
    for name, title_key, sub_key in STRIPS:
        title = strings[title_key]
        sub = strings[sub_key] if sub_key else ""
        images.append(render_close(title, font_path) if name == "OSD_CLOSE"
                      else render_strip(title, sub, font_path))
    locale_strips[stem] = images
    locale_of[stem] = render_glyph(strings["C_OSD_OF"], font_path)

# English aliases keep the visual preview below deterministic.
strip_imgs = locale_strips["en"]
strip_names = [name.lower() for name, _, _ in STRIPS]
glyph_imgs = [render_glyph(g) for g in GLYPHS[:10]] + [locale_of["en"]]

out_c = ["// GENERATED by assets/generators/scan_osd.py - do not hand-edit",
         '#include "scan_osd.h"', ""]
strip_meta = {}
for li, stem in enumerate(LOCALES):
    ident = stem.lower().replace("-", "_")
    strip_meta[stem] = []
    for (enum_name, _, _), im in zip(STRIPS, locale_strips[stem]):
        name = f"osd_{ident}_{enum_name.lower()}"
        w, h, bits = pack_a4(im)
        out_c.append(f"static const uint8_t {name}_a4[] = {{{', '.join(str(b) for b in bits)}}};")
        strip_meta[stem].append((name, w, h))
out_c += ["", "const scan_osd_strip_t scan_osd[][SCAN_OSD_N] = {"]
for stem in LOCALES:
    out_c.append("    {")
    for name, w, h in strip_meta[stem]:
        out_c.append(f"        {{{w}, {h}, {name}_a4}},")
    out_c.append("    },")
out_c += ["};", ""]

digit_imgs = [render_glyph(str(d)) for d in range(10)]
out_c += emit(digit_imgs, [f"glyph_{d}" for d in range(10)],
              "scan_osd_strip_t", "scan_osd_glyph")
out_c.append("")
of_meta = []
for stem in LOCALES:
    ident = stem.lower().replace("-", "_")
    name = f"glyph_of_{ident}"
    w, h, bits = pack_a4(locale_of[stem])
    out_c.append(f"static const uint8_t {name}_a4[] = {{{', '.join(str(b) for b in bits)}}};")
    of_meta.append((name, w, h))
out_c += ["", "const scan_osd_strip_t scan_osd_of[] = {"]
for name, w, h in of_meta:
    out_c.append(f"    {{{w}, {h}, {name}_a4}},")
out_c += ["};"]

with open(ROOT / "main" / "scan_osd.c", "w") as f:
    f.write("\n".join(out_c) + "\n")

enum_names = ", ".join(n for n, _, _ in STRIPS)
hdr = f"""// GENERATED by assets/generators/scan_osd.py - do not hand-edit
#pragma once
#include <stdint.h>

enum {{ {enum_names}, SCAN_OSD_N }};
typedef struct {{
    int w, h;               // landscape strip dims (w along landscape-x)
    const uint8_t *a4;      // 4-bit alpha, row-major, high nibble first
}} scan_osd_strip_t;

extern const scan_osd_strip_t scan_osd[][SCAN_OSD_N];
extern const scan_osd_strip_t scan_osd_glyph[10];   // '0'..'9'
extern const scan_osd_strip_t scan_osd_of[];         // localized word "of"
"""
with open(ROOT / "main" / "scan_osd.h", "w") as f:
    f.write(hdr)

# ---- full-screen mock (landscape 800x480), mirroring the C drawing math ----
random.seed(7)
W, H = 800, 480


def fake_video():
    """Colorful stand-in for a live camera frame (desk-ish blobs + noise)."""
    im = Image.new("RGB", (W, H), (38, 44, 52))
    d = ImageDraw.Draw(im)
    for _ in range(90):
        x, y = random.randint(-80, W), random.randint(-60, H)
        r = random.randint(20, 130)
        c = (random.randint(30, 200), random.randint(40, 170), random.randint(30, 150))
        d.ellipse((x, y, x + r, y + r // 2 + 10), fill=c)
    px = im.load()
    for y in range(H):
        for x in range(0, W, 2):
            n = random.randint(-14, 14)
            r, g, b = px[x, y]
            px[x, y] = (max(0, min(255, r + n)), max(0, min(255, g + n)),
                        max(0, min(255, b + n)))
    return im


def feather_band(im, y0, y1, peak=0.62, edge=12):
    px = im.load()
    for y in range(max(0, y0), min(H, y1)):
        din = min(y - y0, y1 - 1 - y)
        f = peak * (min(din, edge) + 1) / (edge + 1) if din < edge else peak
        for x in range(W):
            r, g, b = px[x, y]
            px[x, y] = (int(r * (1 - f)), int(g * (1 - f)), int(b * (1 - f)))


def blit_alpha(im, strip, cx, cy, tint=(255, 255, 255)):
    im.paste(Image.new("RGB", strip.size, tint), (cx - strip.width // 2, cy),
             strip)


def rounded_bar(d, x0, y, ln, fill_frac, segs=None, seen=0, gate=None,
                col=(53, 208, 127)):
    TH, INS = 22, 4
    d.rounded_rectangle((x0, y, x0 + ln, y + TH), TH // 2, fill=(16, 20, 29),
                        outline=(58, 66, 82))
    if segs:
        gap = 5
        sw = (ln - INS * 2 - gap * (segs - 1)) / segs
        for i in range(segs):
            sx = x0 + INS + i * (sw + gap)
            c = col if i < seen else (34, 40, 52)
            d.rounded_rectangle((sx, y + INS, sx + sw, y + TH - INS),
                                (TH - 2 * INS) // 2, fill=c)
    elif fill_frac > 0:
        fw = max(TH - INS * 2, int((ln - INS * 2) * fill_frac))
        d.rounded_rectangle((x0 + INS, y + INS, x0 + INS + fw, y + TH - INS),
                            (TH - 2 * INS) // 2, fill=col)
    if gate is not None:
        gx = x0 + int(ln * gate)
        d.rectangle((gx - 1, y - 4, gx + 1, y + TH + 4), fill=(232, 238, 247))


def brackets(d, cx, cy, side=340, arm=46, t=4, c=(255, 255, 255, 150)):
    s = side // 2
    for sx, sy in ((-1, -1), (1, -1), (-1, 1), (1, 1)):
        x, y = cx + sx * s, cy + sy * s
        d.rectangle((min(x, x - sx * arm), y - t // 2,
                     max(x, x - sx * arm), y + t // 2), fill=c)
        d.rectangle((x - t // 2, min(y, y - sy * arm),
                     x + t // 2, max(y, y - sy * arm)), fill=c)


def compose(state, subtitle_idx, bar):
    im = fake_video()
    feather_band(im, 28, 104)          # top band (landscape y)
    feather_band(im, 380, 446)         # bottom band
    d = ImageDraw.Draw(im, "RGBA")
    if state in ("OSD_SEARCH", "OSD_SEEN", "OSD_CUTOFF", "OSD_READ"):
        brackets(d, W // 2, H // 2 + 10)
    strip = strip_imgs[[n for n, _, _ in STRIPS].index(state)]
    blit_alpha(im, strip, W // 2, 36)
    blit_alpha(im, strip_imgs[-1], 60, 30)     # × close
    if state == "OSD_READ":                     # live "12 of 34" in real type
        x = W // 2 - 60
        for g in ("1", "2", " ", "of", " ", "3", "4"):
            if g == " ":
                x += 10
                continue
            gi = glyph_imgs[GLYPHS.index(g)]
            blit_alpha(im, gi, x + gi.width // 2, 66)
            x += gi.width + 2
    bar(d)
    return im


mocks = [
    compose("OSD_SEARCH", 0, lambda d: rounded_bar(d, 120, 402, 560, 0)),
    compose("OSD_SEEN", 0, lambda d: rounded_bar(d, 120, 402, 560, 0.08,
                                                 col=(255, 228, 64))),
    compose("OSD_READ", 0, lambda d: rounded_bar(d, 120, 402, 560, 0,
                                                 segs=12, seen=5)),
    compose("OSD_ENT_LOW", 0, lambda d: rounded_bar(d, 120, 402, 560, 0.45,
                                                    gate=0.75,
                                                    col=(242, 184, 75))),
    compose("OSD_ENT_OK", 0, lambda d: rounded_bar(d, 120, 402, 560, 0.85,
                                                   gate=0.75)),
]
sheet = Image.new("RGB", (W, (H + 10) * len(mocks)), (10, 10, 10))
for i, m in enumerate(mocks):
    sheet.paste(m, (0, i * (H + 10)))
sheet.save("/tmp/scan_ui_mock.png")
print("wrote main/scan_osd.c/.h + /tmp/scan_ui_mock.png")
for (n, _, _), im in zip(STRIPS, strip_imgs):
    print(f"  {n}: {im.width}x{im.height}")
