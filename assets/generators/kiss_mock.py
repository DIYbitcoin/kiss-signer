#!/usr/bin/env python3
"""Cyberpunk KISS Signer main-menu THEME (visual shell only — no wallet logic).
Landscape 800x480 to match the game. Emits the DEFAULT mono-white menu as a baked
LVGL RGB565 image (main/kiss_img.{c,h}) shown after the KISS unlock gesture, plus
a preview sheet demoing the switchable accent themes AND the safety status light.

Two independent visual systems:
  * THEME accent  — colors the chrome (brackets, grid, tiles, fingerprint). Shown as a
    SINGLE dot in that color, bottom-right (white dot = mono theme, orange dot = orange…).
  * STATUS light  — a Block/Verify/Warn safety guide, bottom-left. Green = ready / just
    saved, Yellow = default "go slow & verify", Red = stop · read · back up.

    kiss_mock.py                 # the Guition: main/kiss_img.{c,h}, main/tile_lbls.{c,h}
    kiss_mock.py --board ws35    # the Waveshare 3.5in: main/kiss_img_ws35.c only

The 3.5in file defines the same img_wallet against the same kiss_img.h, drawn
at 480x320; main/CMakeLists.txt compiles one of the two into an image, never
both. It gets no header and no label strips of its own: kiss_img.h is shared,
and main.c takes only TILE_LBL_Y from tile_lbls.h, which the wide run writes.
Bake whichever .c was written with tools/bake_art.py afterwards.
"""
import argparse
import os
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

# THE ART IS DRAWN ONCE, ON THE WIDE CANVAS. Every length below is written for
# 800x480, and a smaller board scales it here exactly as main/kiss_board.h
# scales the live chrome that sits on top of it: X for an x or a width, Y for
# a y or a height, integer floor, so the baked skeleton lands under the live
# frames to the pixel. On the wide board both fold to the number itself, and
# nothing there can move. A width is scaled apart from its position and the
# edge is their sum, never the scaled sum. Square things (icons, dots, the
# kiss mark, the grid pitch) take X on both axes, as the live frames do.
# Strokes, blur and type step down by the y-scale (2/3 on the 3.5in), never
# the x-scale (3/5), which would thin a 3px stroke to 1; type stops at 10px.
DW, DH = 800, 480                      # the design canvas
BOARDS = {"guition": (800, 480), "ws35": (480, 320)}
ap = argparse.ArgumentParser(description=__doc__,
                             formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("--board", choices=sorted(BOARDS), default="guition",
                help="which board's canvas to draw on (default: guition)")
ARGS = ap.parse_args()
W, H = BOARDS[ARGS.board]
SUFFIX = "" if ARGS.board == "guition" else "_" + ARGS.board


def X(v):
    return v * W // DW


def Y(v):
    return v * H // DH


def TILE_X(i):
    """A home tile's left edge. The 3.5in's tiles are a little wider than three
    fifths (102 px, 8 px apart) so its 18 px tile words keep their margins; the
    firmware's HOME_TILE_X says the same numbers."""
    return 24 + i * 110 if ARGS.board == "ws35" else X(50) + i * X(180)


def TILE_W():
    return 102 if ARGS.board == "ws35" else X(161)


def S(v):
    """A stroke width or a blur radius: the y-scale, never below one pixel."""
    return max(1, Y(v))


ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "main")
INK = (232, 238, 247)
MUT = (122, 134, 156)
SUB = (176, 188, 205)   # brighter muted — card subtitles must out-read the faint bg grid
CARD = (14, 18, 28)
GRID_PITCH = 46         # background grid spacing (original density)


def font(size, mono=False):
    size = max(10, Y(size))     # 2/3 on the 3.5in, and nothing under 10px reads
    cands = (["/System/Library/Fonts/Menlo.ttc"] if mono else
             ["/System/Library/Fonts/Supplemental/Futura.ttc",
              "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
              "/System/Library/Fonts/Supplemental/Arial.ttf"])
    for p in cands:
        try:
            return ImageFont.truetype(p, size)
        except Exception:
            pass
    return ImageFont.load_default()


def hx(h):
    h = h.lstrip("#"); return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


THEMES = [("Mono", "#E8EEF7"), ("Krux green", "#3DEE8B"),
          ("Cypherpink", "#FF3D9A"), ("Bitcoin orange", "#FF8A1E")]

# Safety status light — independent of the theme accent.
STATUS = {
    "go":      ("READY",   "safe to proceed",       (46, 222, 138)),
    "caution": ("CAUTION", "go slow  ·  verify",    (245, 197, 66)),
    "stop":    ("VERIFY",  "stop · read · back up", (255, 77, 77)),
}


def icon_sign(d, cx, cy, s, A):
    # pencil signing on a baseline: tip lower-left, eraser end upper-right
    ux, uy = 0.7071, -0.7071     # toward the tail (up-right)
    px, py = 0.7071, 0.7071      # perpendicular across the body
    w = s * .15
    tipx, tipy = cx - s * .5, cy + s * .5
    tailx, taily = cx + s * .5, cy - s * .5
    nbx, nby = tipx + ux * s * .26, tipy + uy * s * .26   # base of the sharpened nib
    a1, a2 = (nbx + px * w, nby + py * w), (tailx + px * w, taily + py * w)
    b1, b2 = (nbx - px * w, nby - py * w), (tailx - px * w, taily - py * w)
    d.line([a1, a2], fill=A, width=S(3))                  # body long edges
    d.line([b1, b2], fill=A, width=S(3))
    d.line([a2, b2], fill=A, width=S(3))                  # tail cap (eraser end)
    d.line([a1, (tipx, tipy)], fill=A, width=S(3))        # nib
    d.line([b1, (tipx, tipy)], fill=A, width=S(3))
    fx, fy = tailx - ux * s * .26, taily - uy * s * .26   # ferrule band
    d.line([(fx + px * w, fy + py * w), (fx - px * w, fy - py * w)], fill=A, width=S(3))
    d.line([(cx - s * .62, cy + s * .74), (cx + s * .5, cy + s * .74)], fill=A, width=S(3))  # baseline


def icon_receive(d, cx, cy, s, A):
    b = s * .42
    for ox, oy in [(-1, -1), (1, -1), (-1, 1)]:
        x0, y0 = cx + ox * b - s * .22, cy + oy * b - s * .22
        d.rectangle([x0, y0, x0 + s * .44, y0 + s * .44], outline=A, width=S(3))
        d.rectangle([x0 + s * .15, y0 + s * .15, x0 + s * .29, y0 + s * .29], fill=A)
    d.rectangle([cx + s * .22, cy + s * .22, cx + s * .34, cy + s * .34], fill=A)
    d.rectangle([cx + s * .5, cy + s * .22, cx + s * .62, cy + s * .34], fill=A)


def icon_key(d, cx, cy, s, A):
    # universal vertical key (🔑): round bow on top, stem down, teeth at the bottom
    r = s * .34
    by = cy - s * .4                                                          # bow center (top)
    d.ellipse([cx - r, by - r, cx + r, by + r], outline=A, width=S(3))        # bow
    d.ellipse([cx - r * .38, by - r * .38, cx + r * .38, by + r * .38], outline=A, width=S(2))  # hole
    sy1 = cy + s * .74                                                        # stem bottom (tip)
    d.line([(cx, by + r * .92), (cx, sy1)], fill=A, width=S(3))               # stem
    for ty in (sy1, sy1 - s * .24):                                           # teeth (to the right)
        d.line([(cx, ty), (cx + s * .3, ty)], fill=A, width=S(3))


def icon_settings(d, cx, cy, s, A):
    k = X(7)                                   # knob radius, square: X on both axes
    for i, yy in enumerate((-s * .42, 0, s * .42)):
        d.line([(cx - s * .6, cy + yy), (cx + s * .6, cy + yy)], fill=A, width=S(3))
        kx = cx + (s * .3 if i == 1 else -s * .25 if i == 0 else s * .05)
        d.ellipse([kx - k, cy + yy - k, kx + k, cy + yy + k], fill=CARD, outline=A, width=S(3))


ICONS = [("Sign", "a transaction", icon_sign), ("Receive", "an address", icon_receive),
         ("Keys", "keys & export", icon_key), ("Settings", "device & theme", icon_settings)]

# Kiss-mark logo next to the KISS wordmark: real Twemoji artwork (CC-BY 4.0,
# see assets/twemoji/README.md). Hand-drawn line-art lips were rejected — they
# never read like the emoji at this size. Pasted full-color in render(), not
# through the accent chrome (it keeps its own red).
KISS_MARK = Image.open(os.path.join(ROOT, "assets", "twemoji", "1f48b.png")).convert("RGBA")
KISS_MARK_SIZE = X(36)
KISS_MARK_POS = (X(210), Y(68) - KISS_MARK_SIZE // 2)

BY = Y(DH - 46)  # bottom-row baseline for the status / theme indicators


def live_boxes_ws35(d, struct):
    """The 3.5in's corner brackets and KISS rule, drawn in the live objects' own
    boxes (main.c: the corners at SX(24)/SX(744) by SY(24)/SY(424), SX(32)
    square with 3 px rims; the rule at SX(46), 64, SX(143) by 3). Scaled from
    the wide strokes, the bottom brackets baked two rows under the live ones and
    read doubled, and the rule baked where it no longer sits."""
    k = X(32)
    for bx, by, dx, dy in [(X(24), Y(24), 1, 1), (X(744), Y(24), -1, 1),
                           (X(24), Y(424), 1, -1), (X(744), Y(424), -1, -1)]:
        hy = by if dy > 0 else by + k - 3
        vx = bx if dx > 0 else bx + k - 3
        d.rectangle([bx, hy, bx + k - 1, hy + 2], fill=struct)
        d.rectangle([vx, by, vx + 2, by + k - 1], fill=struct)
    d.rectangle([X(46), 64, X(46) + X(143) - 1, 66], fill=struct)


def accent_art(d, struct, theme_col, status_col, status=True, parts="all",
               theme_dot=True):
    """struct = chrome stroke color (opaque sharp pass, or alpha tuple for glow pass).
    theme_col / status_col are always drawn in their true colors.
    status=False skips the status pill: the firmware bake leaves that corner to a
    live build-identity label (a light that never changes is a fake light).
    parts: "all" (preview sheet) / "icons" / "frames" — the firmware bake draws
    icons white and frames DIM, because the frames are re-drawn LIVE in the
    active theme's accent (LVGL borders + shadow glow), which is how theme
    switching recolors the home without re-baking 768KB per theme.
    theme_dot=False skips the baked dot+name (live indicator owns that corner)."""
    tw = 160
    if parts in ("all", "frames"):
        if ARGS.board == "ws35":
            live_boxes_ws35(d, struct)
        else:
            for (ox, oy, dx, dy) in [(26, 26, 1, 1), (DW - 26, 26, -1, 1), (26, DH - 26, 1, -1), (DW - 26, DH - 26, -1, -1)]:
                d.line([(X(ox), Y(oy)), (X(ox) + dx * X(26), Y(oy))], fill=struct, width=S(3))
                d.line([(X(ox), Y(oy)), (X(ox), Y(oy) + dy * Y(26))], fill=struct, width=S(3))
            # The underline and the frames in main.c's own terms (position, size),
            # so the skeleton and the live border scale to the same pixels: an
            # inclusive right edge is the scaled position plus the scaled size less
            # one, never the scaled sum, which can land a pixel past the live frame.
            d.line([(X(46), Y(96)), (X(46) + X(143) - 1, Y(96))], fill=struct, width=S(3))
        if ARGS.board == "ws35":
            # the firmware's HOME_CHIP_* box: wider for its 23 px code
            d.rounded_rectangle([300, 22, 300 + 140 - 1, 22 + 36 - 1],
                                X(10), outline=struct, width=S(2))
        else:
            d.rounded_rectangle([X(566), Y(40), X(566) + X(195) - 1, Y(40) + Y(47) - 1],
                                X(10), outline=struct, width=S(2))
        for i in range(4):
            x0 = TILE_X(i)
            d.rounded_rectangle([x0, Y(150), x0 + TILE_W() - 1, Y(150) + Y(183) - 1],
                                X(12), outline=struct, width=S(2))
    if parts in ("all", "icons"):
        for i in range(4):
            x0 = TILE_X(i)
            cx = x0 + TILE_W() / 2 if ARGS.board == "ws35" else x0 + X(tw) / 2
            ICONS[i][2](d, cx, Y(212), X(46), struct)
    # status light pill (bottom-left) — preview sheet only, see status=False
    if status:
        d.rounded_rectangle([X(44), BY - Y(22), X(250), BY + Y(22)], X(22), outline=status_col, width=S(2))
        d.ellipse([X(62) - X(9), BY - X(9), X(62) + X(9), BY + X(9)], fill=status_col)
    # theme dot (bottom-right)
    if theme_dot:
        d.ellipse([X(684) - X(7), BY - X(7), X(684) + X(7), BY + X(7)], fill=theme_col)
        d.ellipse([X(684) - X(11), BY - X(11), X(684) + X(11), BY + X(11)], outline=theme_col, width=S(2))


def render(active, status="caution", fp=None, grid_a=44, bake_chip=True,
           bake_status=True, bake_tile_labels=True, live_frames=False):
    # live_frames=True (firmware bake): icons stay white w/ glow; the frame
    # strokes bake DIM (fallback skeleton) and the theme dot is skipped — the
    # firmware draws frames/dot live in the active accent.
    # grid_a = solid-grid opacity 0-255 (lower = more transparent)
    # bake_chip=False leaves the fingerprint chip area BLANK: the firmware bake uses it
    # because the chip is dynamic (EMPTY vs live fingerprint) and is drawn as LVGL labels.
    A = hx(THEMES[active][1])
    slab, ssub, scol = STATUS[status]
    base = Image.new("RGBA", (W, H), (7, 10, 16, 255))
    g = ImageDraw.Draw(base)
    if grid_a:   # SOLID background grid (0 = off). Draw on a SEPARATE transparent layer, then
                 # alpha_composite. Drawing an alpha fill straight onto the opaque base does NOT
                 # blend — PIL paints it full-strength and drops the alpha, which is why every
                 # grid_a value looked identically bright before. grid_a is now a REAL opacity
                 # dial (0-255): lower = more transparent.
        gl_grid = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        gg = ImageDraw.Draw(gl_grid)
        for x in range(0, W, X(GRID_PITCH)):
            gg.line([(x, 0), (x, H)], fill=(*A, grid_a), width=1)
        for y in range(0, H, X(GRID_PITCH)):
            gg.line([(0, y), (W, y)], fill=(*A, grid_a), width=1)
        # Fade the grid out toward the edges/corners so the four corner items (KISS logo,
        # fingerprint, status pill, theme dot) sit on clean darkness. Elliptical radial mask:
        # full grid mid-screen, gone at the corners.
        gy2, gx2 = np.mgrid[0:H, 0:W]
        nd = np.sqrt(((gx2 - W / 2) / (W / 2)) ** 2 + ((gy2 - H / 2) / (H / 2)) ** 2)
        fade = np.clip((1.15 - nd) / 0.5, 0, 1)          # 1 until nd~0.65, ->0 by nd~1.15 (corners)
        ga = (np.asarray(gl_grid.getchannel("A")).astype(np.float32) * fade).astype(np.uint8)
        gl_grid.putalpha(Image.fromarray(ga))
        base.alpha_composite(gl_grid)
    yy, xx = np.mgrid[0:H, 0:W]
    vig = (np.clip((np.sqrt((xx - W / 2) ** 2 + (yy - H / 2) ** 2) / X(520) - 0.5) / 0.5, 0, 1) * 150).astype(np.uint8)
    base.alpha_composite(Image.fromarray(np.dstack([np.zeros((H, W, 3), np.uint8), vig]), "RGBA"))
    gl = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    accent_art(ImageDraw.Draw(gl), (*A, 200), theme_col=A, status_col=scol,
               status=bake_status, parts="icons" if live_frames else "all",
               theme_dot=not live_frames)
    base.alpha_composite(gl.filter(ImageFilter.GaussianBlur(S(6))))
    d = ImageDraw.Draw(base)
    if live_frames:
        DIMF = (58, 66, 84)      # quiet skeleton; live accent frames sit on top
        accent_art(d, DIMF, theme_col=A, status_col=scol, status=False,
                   parts="frames", theme_dot=False)
        accent_art(d, A, theme_col=A, status_col=scol, status=bake_status,
                   parts="icons", theme_dot=False)
    else:
        accent_art(d, A, theme_col=A, status_col=scol, status=bake_status)
    # header. The 3.5in's KISS sits two rows above the scaled 29, its tagline
    # four below, and main.c drops the rule between them two: the rule sat 2 px
    # under the word and 3 px over the tagline. It keeps 6 px from each now,
    # and the tagline's descenders still end 13 px above the tiles.
    d.text((X(44), 27 if ARGS.board == "ws35" else Y(44)), "KISS", font=font(44), fill=INK)
    if ARGS.board == "ws35":
        # The 3.5in draws the tagline in the firmware's own mono face at the
        # smallest size its screens set text at, and a step brighter: scaled
        # like the rest it was 10 px Menlo in the muted grey, which the bench
        # called almost impossible to read and not the device's typeface.
        tag = ImageFont.truetype(os.path.join(ROOT, "tools/fonts/vendor/IoskeleyMono-Medium-ascii.ttf"), 14)
        d.text((X(48), 70), "offline bitcoin signing device", font=tag, fill=SUB)
    else:
        d.text((X(48), Y(100)), "offline bitcoin signing device", font=font(15, mono=True), fill=MUT)
    # kiss-mark logo: soft glow pass then sharp, same treatment as the chrome
    mark = KISS_MARK.resize((KISS_MARK_SIZE, KISS_MARK_SIZE), Image.LANCZOS)
    ml = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ml.alpha_composite(mark, KISS_MARK_POS)
    base.alpha_composite(ml.filter(ImageFilter.GaussianBlur(S(6))))
    base.alpha_composite(mark, KISS_MARK_POS)
    # fingerprint chip — EMPTY until a wallet exists (skipped entirely for the firmware
    # bake: live LVGL labels own this area)
    if bake_chip:
        if fp:
            d.text((X(584), Y(48)), "◇ " + fp, font=font(20, mono=True), fill=A)
            d.text((X(584), Y(70)), "fingerprint", font=font(12, mono=True), fill=MUT)
        else:
            d.text((X(584), Y(48)), "◇ EMPTY", font=font(20, mono=True), fill=MUT)
            d.text((X(584), Y(70)), "no wallet yet", font=font(12, mono=True), fill=MUT)
    # tiles (labels skipped for the firmware bake: they live as RGB565A8 image
    # objects so the unlock can settle them in — see tile_lbls.c below)
    tw = 160
    if bake_tile_labels:
        for i, (lab, sub, _) in enumerate(ICONS):
            x0 = X(50) + i * X(tw + 20)
            d.text((x0 + (X(tw) - d.textlength(lab, font=font(23))) / 2, Y(268)), lab, font=font(23), fill=INK)
            d.text((x0 + (X(tw) - d.textlength(sub, font=font(13, mono=True))) / 2, Y(300)), sub, font=font(13, mono=True), fill=SUB)
    # status light label (preview sheet only)
    if bake_status:
        d.text((X(84), BY - Y(14)), slab, font=font(20), fill=scol)
        d.text((X(84), BY + Y(8)), ssub, font=font(11, mono=True), fill=MUT)
    # theme dot label (preview sheet only — the firmware's is live)
    if not live_frames:
        nm = THEMES[active][0].upper()
        fn = font(15, mono=True)
        d.text((X(704), BY - Y(9)), nm, font=fn, fill=INK)
        d.text((X(704), BY - Y(28)), "theme", font=font(10, mono=True), fill=MUT)
    return base.convert("RGB")


if not SUFFIX:
    # preview sheet — demo the themes (per row) AND the three status states
    ROWS = [(0, "caution", None), (1, "go", "7F3A·9C21"),
            (2, "stop", "B2C8·41DE"), (3, "caution", "5A19·7766")]
    pad, lab_h = 10, 30
    sheet = Image.new("RGB", (W + 2 * pad, (H + lab_h) * len(ROWS) + pad), (10, 12, 18))
    sd = ImageDraw.Draw(sheet)
    for r, (ti, st, fp) in enumerate(ROWS):
        name, hexv = THEMES[ti]
        y = pad + r * (H + lab_h)
        sd.text((pad + 4, y + 6), f"■ {name} theme   ·   status: {st.upper()}", font=font(16, mono=True), fill=hx(hexv))
        sheet.paste(render(ti, st, fp), (pad, y + lab_h))
    sheet.save("/tmp/kiss_mock.png")
    print("saved /tmp/kiss_mock.png", sheet.size)

# emit the DEFAULT baked menu: mono theme, chip area blank (dynamic: EMPTY vs live
# fingerprint), NO status pill (that corner is the live build-identity label now —
# the baked CAUTION never changed, which made it a fake status light) and NO tile
# labels (they settle in as live image objects on unlock)
img = render(0, "caution", None, bake_chip=False, bake_status=False,
             bake_tile_labels=False, live_frames=True)
if SUFFIX:
    # a narrow board gets no sheet: the bare bake is its preview
    img.save("/tmp/kiss_mock%s.png" % SUFFIX)
    print("saved /tmp/kiss_mock%s.png" % SUFFIX, img.size)
rgb = np.array(img)
v = (((rgb[..., 0] >> 3).astype(np.uint16) << 11) | ((rgb[..., 1] >> 2).astype(np.uint16) << 5) | (rgb[..., 2] >> 3))
data = np.dstack([(v & 0xFF).astype(np.uint8), (v >> 8).astype(np.uint8)]).reshape(-1)
with open(OUT + "/kiss_img%s.c" % SUFFIX, "w") as f:
    f.write('#include "lvgl.h"\n\n')
    f.write("static const uint8_t kiss_map[] = {%s};\n\n" % ",".join(map(str, data.tolist())))
    f.write("const lv_image_dsc_t img_wallet = {\n")
    f.write("  .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,\n")
    f.write("             .flags = 0, .w = %d, .h = %d, .stride = %d },\n" % (W, H, W * 2))
    f.write("  .data_size = sizeof(kiss_map), .data = kiss_map,\n};\n")
print("wrote main/kiss_img%s.c (img_wallet %dx%d: mono theme, no status pill, EMPTY chip)"
      % (SUFFIX, W, H))
if SUFFIX:
    raise SystemExit(0)   # the header and the label strips below are the wide run's

with open(OUT + "/kiss_img.h", "w") as f:
    f.write('#pragma once\n#include "lvgl.h"\nextern const lv_image_dsc_t img_wallet;\n')

# ---- tile label strips (RGB565A8, same typography as the old bake) ----
# Full tile width so main.c places them at the exact baked coords: x = 50+i*180,
# y = TILE_LBL_Y. Title baseline/colors identical to the previous baked text.
TILE_LBL_Y = 262            # strip origin on screen (title was baked at y=268)
TILE_LBL_H = 60
lc = ['#include "lvgl.h"', ""]
metas = []
for i, (lab, sub, _) in enumerate(ICONS):
    im = Image.new("RGBA", (160, TILE_LBL_H), (0, 0, 0, 0))
    d2 = ImageDraw.Draw(im)
    f1, f2 = font(23), font(13, mono=True)
    d2.text(((160 - d2.textlength(lab, font=f1)) / 2, 268 - TILE_LBL_Y),
            lab, font=f1, fill=INK)
    d2.text(((160 - d2.textlength(sub, font=f2)) / 2, 300 - TILE_LBL_Y),
            sub, font=f2, fill=SUB)
    a = np.array(im)
    r16 = a[..., 0].astype(np.uint16)
    g16 = a[..., 1].astype(np.uint16)
    b16 = a[..., 2].astype(np.uint16)
    al = a[..., 3].astype(np.uint8)
    v16 = (r16 >> 3) << 11 | (g16 >> 2) << 5 | (b16 >> 3)
    data = np.concatenate([
        np.dstack([(v16 & 0xFF).astype(np.uint8),
                   (v16 >> 8).astype(np.uint8)]).reshape(-1),
        al.reshape(-1)])
    lc.append("static const uint8_t tile_lbl%d_map[] = {%s};" %
              (i, ",".join(str(int(x)) for x in data)))
    metas.append(i)
lc.append("")
lc.append("const lv_image_dsc_t img_tile_lbls[4] = {")
for i in metas:
    lc.append("  {.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565A8,")
    lc.append("              .flags = 0, .w = 160, .h = %d, .stride = 320}," % TILE_LBL_H)
    lc.append("   .data_size = sizeof(tile_lbl%d_map), .data = tile_lbl%d_map}," % (i, i))
lc.append("};")
with open(OUT + "/tile_lbls.c", "w") as f:
    f.write("// GENERATED by assets/generators/kiss_mock.py - do not hand-edit\n")
    f.write("\n".join(lc) + "\n")
with open(OUT + "/tile_lbls.h", "w") as f:
    f.write("// GENERATED by assets/generators/kiss_mock.py - do not hand-edit\n"
            "#pragma once\n#include \"lvgl.h\"\n\n"
            "#define TILE_LBL_Y %d   // screen y for every strip; x = 50 + i*180\n"
            "extern const lv_image_dsc_t img_tile_lbls[4];\n" % TILE_LBL_Y)
print("wrote main/tile_lbls.c/.h (4 RGB565A8 label strips, 160x%d)" % TILE_LBL_H)
