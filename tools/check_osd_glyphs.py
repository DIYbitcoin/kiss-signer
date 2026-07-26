#!/usr/bin/env python3
"""Fail if any camera-overlay string needs a glyph its baked font lacks.

The live camera bypasses LVGL, so every scanner caption is a bitmap baked at
build time by assets/generators/scan_osd.py. A baked strip has perfectly
normal dimensions whether or not the glyphs inside it exist: a missing one is
drawn as a tofu box, silently, and the generated C looks identical either way.

That is not hypothetical. The generator used macOS "Arial Rounded Bold", which
has no Cyrillic and no Vietnamese, so every Russian camera overlay shipped as
a row of boxes and Vietnamese lost its stacked diacritics. Nothing caught it
for months because nothing was comparing the text against the font -- the only
signal was looking at the pixels.

This is that comparison, as a test. Run it in CI so a future font swap, or a
translation reaching for a character outside the current coverage, fails loudly
instead of shipping.
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GEN = ROOT / "assets/generators/scan_osd.py"


def gen_config():
    """Pull LOCALES, STRIPS, FONT_LAT and FONT_CJK out of the generator.

    Importing it would render every strip and rewrite main/scan_osd.c as a side
    effect, so read the values instead. Keeping one source of truth matters
    more here than elegance: a check that drifts from the generator it guards
    is worse than no check.
    """
    src = GEN.read_text(encoding="utf-8")
    ns = {"Path": Path, "ROOT": ROOT, "__file__": str(GEN)}
    for name in ("LOCALES", "STRIPS", "FONT_LAT", "FONT_CJK"):
        m = re.search(rf"^{name} = (.+?)(?=^\w|\Z)", src, re.S | re.M)
        if not m:
            sys.exit(f"cannot find {name} in {GEN}")
        exec(f"{name} = {m.group(1).strip()}", ns)
    return ns["LOCALES"], ns["STRIPS"], ns["FONT_LAT"], ns["FONT_CJK"]


def coverage(path):
    """Codepoints the font covers, or None when the font is not on this host.

    The CJK sources are macOS system fonts, so on Linux CI they are absent and
    those locales cannot be checked at all -- the generator cannot run there
    either. Report that as an explicit SKIP rather than crashing or, worse,
    passing quietly. The font that actually shipped tofu is the Latin/Cyrillic
    one, and that lives in the repo's LVGL component, so CI always checks it.
    """
    from fontTools.ttLib import TTFont
    if not Path(path).exists():
        return None
    f = TTFont(path, fontNumber=0)
    cps = set()
    for t in f["cmap"].tables:
        cps |= set(t.cmap.keys())
    return cps


def main():
    locales, strips, font_lat, font_cjk = gen_config()
    keys = [k for s in strips for k in s[1:] if k] + ["C_OSD_OF"]

    cache = {}
    bad = 0
    checked = skipped = 0
    for loc in locales:
        path = font_cjk.get(loc, font_lat)
        if path not in cache:
            cache[path] = coverage(path)
        cps = cache[path]
        if cps is None:
            print(f"SKIP {loc}: {path} not on this host")
            skipped += 1
            continue
        checked += 1
        data = json.loads((ROOT / "i18n" / f"{loc}.json").read_text(encoding="utf-8"))
        for k in keys:
            txt = data.get(k)
            if not txt:
                continue
            miss = sorted({c for c in txt
                           if ord(c) not in cps and c not in "\n\t "})
            if miss:
                bad += 1
                print(f"FAIL {loc}/{k}: {Path(path).name} lacks "
                      f"{' '.join(f'{c!r} U+{ord(c):04X}' for c in miss)}")

    n = checked * len(keys)
    if bad:
        print(f"\n{bad} of {n} overlay strings would render tofu on the camera.")
        return 1
    if not checked:
        print("no font available to check against")
        return 1
    tail = f" ({skipped} locale(s) skipped, font not on this host)" if skipped else ""
    print(f"ok: {n} camera-overlay strings across {checked} locale(s), "
          f"every glyph present in its baked font{tail}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
