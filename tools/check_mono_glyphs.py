#!/usr/bin/env python3
"""Which strings would the mono faces refuse to draw?

The screen-system pass sets every string on its screens in IoskeleyMono, and
those faces carry ASCII 0x20-0x7E plus middle dot, bullet and ellipsis --
nothing else. A glyph outside that set draws LVGL's placeholder box on the
device, silently in the source: the string looks fine in the JSON and wrong on
the glass. So the constraint is a COPY constraint, and this is its gate.

The usual leaks are typography, not language: an em dash where the house style
wants a middle dot or a full stop, a curly quote for a straight one, a `x`
written as the multiplication sign, a `>` written as a single guillemet.

    python3 tools/check_mono_glyphs.py          en.json (the English-only rule)
    python3 tools/check_mono_glyphs.py --all    every locale (the sweep)

Exit 1 on any finding. The check proves it can fire before it reports a clean
sweep, and refuses to report one if it cannot.
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# ASCII 0x20-0x7E plus the three marks gen_fonts.sh gives the mono faces.
# Newline is a layout instruction, not a glyph, and never reaches the font.
ALLOWED = {chr(c) for c in range(0x20, 0x7F)} | {"·", "•", "…", "\n"}

# What to write instead, for the leaks that have names.
INSTEAD = {
    "—": "an em dash: use a middle dot or a full stop",
    "–": "an en dash: use a middle dot or a full stop",
    "‘": "a curly quote: use '",
    "’": "a curly quote: use '",
    "“": "a curly quote: use '",
    "”": "a curly quote: use '",
    "×": "a multiplication sign: use x",
    "›": "a guillemet: use >",
    "‹": "a guillemet: use <",
    " ": "a no-break space: use a space",
}


def audit(strings):
    """(key, ch) pairs for every glyph the mono faces cannot draw."""
    out = []
    for key, val in strings.items():
        if not isinstance(val, str):
            continue
        for ch in val:
            if ch not in ALLOWED:
                out.append((key, ch))
    return out


def selftest():
    """The check must fire on a known-bad string and stay quiet on a good one."""
    bad = audit({"K": "a — dash"})
    good = audit({"K": "a · dot, a • bullet, an … ellipsis"})
    if len(bad) != 1 or good:
        print("SELFTEST FAILED: the check no longer fires; not reporting")
        sys.exit(2)


def main():
    selftest()
    if "--all" in sys.argv[1:]:
        files = sorted((ROOT / "i18n").glob("*.json"))
    else:
        files = [ROOT / "i18n" / "en.json"]

    findings = 0
    for f in files:
        for key, ch in audit(json.loads(f.read_text(encoding="utf-8"))):
            hint = INSTEAD.get(ch, "outside the mono set")
            print(f"{f.name} {key}: U+{ord(ch):04X} {ch!r} -- {hint}")
            findings += 1
    if findings:
        print(f"{findings} glyph(s) the mono faces would draw as a box")
        sys.exit(1)
    scope = "all locales" if len(files) > 1 else "en"
    print(f"mono glyphs clean ({scope}, {len(files)} file(s))")


if __name__ == "__main__":
    main()
