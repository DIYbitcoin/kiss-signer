#!/usr/bin/env python3
"""Shared synthwave / outrun sunset-island hero scene for the FRUIT ISLAND menu and
game-over backdrops. Big banded retro sun with a hot core, a glowing horizon line, a
bold perspective grid, a vertical sun-pillar reflection, and a neon-rimmed island with
graceful palm silhouettes. Tuned for a 480-wide portrait canvas; scales with width."""
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

DARK = (9, 6, 20)


def fill_holes(m):
    """Fill fully-ENCLOSED holes in an L-mode mask (e.g. the A's near-closed triangle
    counter in the stroked logo plate) so the drop shadow can't peek through them.
    Pure flood-fill from the border: outer contours and gaps between letters are
    untouched -- unlike a morphological close, letter shapes stay pixel-identical."""
    a = np.array(m)
    free = a < 128
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


def _grad4(t, c0, c1, c2, c3):
    """Piecewise vertical gradient over 4 stops; t in [0,1] array shape (...,1)."""
    a = c0 + (c1 - c0) * np.clip(t / 0.34, 0, 1)
    a = np.where(t > 0.34, c1 + (c2 - c1) * np.clip((t - 0.34) / 0.33, 0, 1), a)
    a = np.where(t > 0.67, c2 + (c3 - c2) * np.clip((t - 0.67) / 0.33, 0, 1), a)
    return a


def synthwave_scene(img, W, H, CX, HZ, xx, yy, island=True):
    """Composite the scene onto `img` (an RGBA sky-gradient base) and return it."""
    SUNX = CX
    s = min(W, H) / 480.0   # size off the shorter side -> consistent hero in portrait OR landscape

    # ---- stars (upper sky) ----
    rng = np.random.default_rng(7)
    st = Image.new("RGBA", (W, H), (0, 0, 0, 0)); sd = ImageDraw.Draw(st)
    for _ in range(int(160 * s)):
        sx, sy = rng.uniform(0, W), rng.uniform(0, HZ - 70)
        r = rng.uniform(0.4, 1.8); a = int(rng.uniform(40, 215))
        sd.ellipse([sx - r, sy - r, sx + r, sy + r], fill=(255, 255, 255, a))
    img = Image.alpha_composite(img, st)

    # ---- big retro 'outrun' sun: hot white core -> gold -> orange -> magenta, banded ----
    R = int(182 * s); SCY = HZ - int(120 * s)
    rr = np.sqrt((xx - SUNX) ** 2 + (yy - SCY) ** 2)
    t = np.clip((yy - (SCY - R)) / (2 * R), 0, 1)[..., None]
    col = _grad4(t, np.array([255, 252, 236.]), np.array([255, 188, 92.]),
                 np.array([255, 92, 70.]), np.array([255, 28, 120.]))
    alpha = (rr <= R).astype(float)
    yb = SCY - R * 0.02; gap = 3.0 * s; sol = 20.0 * s         # bands: gaps grow, solids shrink downward
    while yb < SCY + R:
        alpha[(yy >= yb) & (yy < yb + gap)] = 0.0
        yb += gap + sol; gap += 1.7 * s; sol *= 0.85
    sun = Image.fromarray(np.dstack([np.clip(col, 0, 255), alpha * 255]).astype(np.uint8), "RGBA")

    halo = np.clip(1 - rr / (R * 2.2), 0, 1) ** 2              # warm bloom behind the sun
    hl = np.zeros((H, W, 4), np.uint8)
    hl[..., 0] = 255; hl[..., 1] = 120; hl[..., 2] = 130; hl[..., 3] = (halo * 190).astype(np.uint8)
    img = Image.alpha_composite(img, Image.fromarray(hl, "RGBA"))
    img = Image.alpha_composite(img, sun)
    core = np.clip(1 - rr / (R * 0.55), 0, 1) ** 2             # hot white core glow
    cg = np.zeros((H, W, 4), np.uint8)
    cg[..., 0] = 255; cg[..., 1] = 246; cg[..., 2] = 222; cg[..., 3] = (core * 200).astype(np.uint8)
    img = Image.alpha_composite(img, Image.fromarray(cg, "RGBA"))

    # ---- glowing horizon line where sun meets sea ----
    hb = np.exp(-((yy - HZ) / (9.0 * s)) ** 2)
    hg = np.zeros((H, W, 4), np.uint8)
    hg[..., 0] = 255; hg[..., 1] = 214; hg[..., 2] = 170; hg[..., 3] = (hb * 215).astype(np.uint8)
    img = Image.alpha_composite(img, Image.fromarray(hg, "RGBA"))

    # ---- bold synthwave perspective grid on the water ----
    grid = Image.new("RGBA", (W, H), (0, 0, 0, 0)); gd = ImageDraw.Draw(grid)
    for k in range(-18, 19):
        gd.line([(SUNX, HZ), (SUNX + k * int(58 * s), H)], fill=(214, 70, 198, 120), width=1)
    yk = HZ + 6; step = 6.0
    while yk < H:
        gd.line([(0, yk), (W, yk)], fill=(120, 224, 232, 120), width=1)
        yk += step; step *= 1.20
    img = Image.alpha_composite(img, grid.filter(ImageFilter.GaussianBlur(0.4)))

    # ---- vertical sun-pillar + banded reflection on the water ----
    refl = Image.new("RGBA", (W, H), (0, 0, 0, 0)); rfd = ImageDraw.Draw(refl)
    yb = HZ + 2; bw = R * 0.95; aa = 200.0
    while yb < H and bw > 8:
        rfd.line([(SUNX - bw / 2, yb), (SUNX + bw / 2, yb)], fill=(255, 132, 116, int(aa)), width=3)
        yb += 7; bw -= 6.0 * s; aa -= 6.0
    pil = ((1 - np.clip(np.abs(xx - SUNX) / (26 * s), 0, 1)) *
           np.clip((H - yy) / (H - HZ), 0, 1) * (yy > HZ))     # narrow bright column
    pl = np.zeros((H, W, 4), np.uint8)
    pl[..., 0] = 255; pl[..., 1] = 220; pl[..., 2] = 180; pl[..., 3] = (pil * 150).astype(np.uint8)
    img = Image.alpha_composite(img, refl.filter(ImageFilter.GaussianBlur(1)))
    img = Image.alpha_composite(img, Image.fromarray(pl, "RGBA").filter(ImageFilter.GaussianBlur(1.5)))

    if not island:
        return img

    # ---- LIT tropical island: shaded green canopy -> warm sand, brown-trunk green-frond palms ----
    cxi, halfw, peak = CX, int(184 * s), int(40 * s)
    prof_h = (np.cos(np.clip((xx - cxi) / halfw, -1, 1) * np.pi / 2) ** 0.7) * peak
    top = HZ - prof_h
    bot = HZ + int(20 * s)
    inside = (yy >= top) & (yy <= bot) & (np.abs(xx - cxi) <= halfw)
    depth = np.clip((yy - top) / np.maximum(bot - top, 1e-6), 0, 1)
    green = np.array([36, 64, 44.]); sand = np.array([158, 122, 76.])
    land = green[None, None, :] * (1 - depth[..., None]) + sand[None, None, :] * depth[..., None]
    lit = np.clip((cxi + halfw - xx) / (2 * halfw), 0, 1)               # sunset light from the left
    land = land * (0.58 + 0.56 * lit[..., None])
    land += np.array([86, 44, 16])[None, None, :] * (lit * (1 - depth))[..., None] * 0.5  # warm foliage glow
    beach = np.clip((depth - 0.82) / 0.18, 0, 1)
    land = land * (1 - beach[..., None]) + np.array([222, 188, 130])[None, None, :] * beach[..., None]
    isl = Image.fromarray(np.dstack([np.clip(land, 0, 255).astype(np.uint8),
                                     np.where(inside, 255, 0).astype(np.uint8)]), "RGBA")
    di = ImageDraw.Draw(isl)
    prof = [(cxi + tt * halfw, HZ - peak * (np.cos(tt * np.pi / 2) ** 0.7)) for tt in np.linspace(-1, 1, 260)]
    di.line(prof, fill=(255, 206, 142, 205), width=2)                  # warm sunset crest rim

    TRUNK = (96, 64, 36, 255); TRUNK_LIT = (150, 108, 60, 255)
    LEAF = (46, 104, 52, 255); LEAF_LIT = (148, 200, 100, 255)

    def palm(px, base_y, ps, lean):
        ps *= s
        n = 22; th = 132 * ps                                  # curved, tapering brown trunk
        tp = [(px + lean * 28 * ps * (u ** 1.35) + lean * 6 * ps * np.sin(u * np.pi), base_y - th * u)
              for u in np.linspace(0, 1, n)]
        for i in range(n - 1):
            di.line([tp[i], tp[i + 1]], fill=TRUNK, width=max(2, int((1.1 - i / n) * 8.0 * ps)))
        for i in range(n - 1):                                 # lit edge on the sun-facing side
            di.line([(tp[i][0] - 2 * ps, tp[i][1]), (tp[i + 1][0] - 2 * ps, tp[i + 1][1])],
                    fill=TRUNK_LIT, width=max(1, int((1.1 - i / n) * 3.0 * ps)))
        cxp, cyp = tp[-1]

        # broad drooping fronds as filled green leaves with a lit midrib
        def frond(ang, L, halfw):
            v = np.linspace(0, 1, 16)
            sx = cxp + np.sin(ang) * L * v
            sy = cyp - np.cos(ang) * L * v * 0.58 + (v ** 2.4) * L * 0.95
            dx = np.gradient(sx); dy = np.gradient(sy)
            ln = np.hypot(dx, dy) + 1e-6
            nx, ny = -dy / ln, dx / ln
            ww = halfw * np.sin(v * np.pi) ** 0.7                       # 0 -> wide -> 0 (leaf taper)
            left = np.stack([sx + nx * ww, sy + ny * ww], 1)
            right = np.stack([sx - nx * ww, sy - ny * ww], 1)
            poly = np.concatenate([left, right[::-1]])
            di.polygon([(float(a), float(b)) for a, b in poly], fill=LEAF)
            di.line([(float(sx[k]), float(sy[k])) for k in range(16)], fill=LEAF_LIT, width=max(1, int(1.6 * ps)))

        for ang in (-1.5, -0.95, -0.42, 0.25, 0.85, 1.45):
            frond(ang, 60 * ps, 7.5 * ps)
        frond(-0.1, 42 * ps, 6.0 * ps)                          # short upright center frond for fullness
        di.ellipse([cxp - 4 * ps, cyp - 2 * ps, cxp + 4 * ps, cyp + 6 * ps], fill=TRUNK)  # crown nub

    palm(cxi - int(104 * s), HZ - int(2 * s), 1.16, -1)
    palm(cxi + int(2 * s),   HZ - int(12 * s), 0.92, 1)
    palm(cxi + int(104 * s), HZ - int(2 * s), 1.08, 1)

    # soft warm sunset glow where the island meets the water, then the lit island on top
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(glow).line(prof, fill=(255, 184, 122, 165), width=5)
    img = Image.alpha_composite(img, glow.filter(ImageFilter.GaussianBlur(6)))
    img = Image.alpha_composite(img, isl)
    return img
