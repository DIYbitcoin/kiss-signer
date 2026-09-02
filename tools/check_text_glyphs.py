#!/usr/bin/env python3
"""Every codepoint the 21 locales put on screen has a glyph in the fonts.

check_glyphs.py asks this of the SYMS icons and check_mono_glyphs.py of the
mono faces, and kiss_theme.h states the reason twice in its own words: a
codepoint missing from the generated fonts draws a blank box about half a line
wide, and it draws it IDENTICALLY in the simulator. No frame, no walk and no
overlap check has ever had an opinion about it.

That argument is about ICONS and it is exactly as true of TEXT, which nothing
asked. It went unasked through a sweep that rewrote 4,800 strings across
twenty locales -- and the sweep leaned on the fonts' coverage about a hundred
times, rewording around characters the CJK sets do not carry. Every one of
those calls was made by hand against tools/fonts/glyphs_*.txt, which is the
SOURCE LIST and not the built font: for the Latin faces the two are not even
related, because gen_fonts.sh builds those from a hardcoded range list and
glyphs_lat.txt had no reader at all.

So this reads the built faces. It parses the cmaps out of main/font_kiss_*.c
-- dense FORMAT0_TINY ranges and sparse unicode_lists both -- and asserts that
every codepoint in i18n/*.json resolves in at least one of them.

Two things it deliberately does NOT check, so the next person does not read
more into a green run than is there:

  * WHICH face. A locale's text is drawn in its own script's face, but the
    picker is a runtime decision in kiss_theme.c and modelling it here would
    be a second implementation of it. Coverage anywhere is the floor: below
    it the glyph cannot draw at all.
  * Whether the glyph is INKED. U+0020 is legitimately a zero-size box, and
    so is every space-like codepoint, so an empty-bitmap rule would need its
    own allow list to say anything. The blank-box failure this exists for is
    the ABSENT codepoint, not the empty one.
"""
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def cmap_codepoints(path):
    """Every codepoint one built face can draw."""
    s = open(path, encoding="utf-8", errors="ignore").read()
    cps = set()
    for m in re.finditer(
            r"\.range_start = (\d+), \.range_length = (\d+), "
            r"\.glyph_id_start = \d+,\s*\n\s*\.unicode_list = (\w+)", s):
        start, length, ul = int(m.group(1)), int(m.group(2)), m.group(3)
        if ul == "NULL":
            cps |= set(range(start, start + length))
            continue
        # sparse: the list holds OFFSETS from range_start, not codepoints
        lm = re.search(r"static const uint\d+_t %s\[\] = \{(.*?)\};" % ul,
                       s, re.S)
        if lm:
            for n in re.findall(r"0x[0-9a-fA-F]+|\d+", lm.group(1)):
                cps.add(start + int(n, 0))
    return cps


def used_codepoints():
    """{codepoint: {locale, ...}} for every string the device can show."""
    used = {}
    for f in sorted(glob.glob(os.path.join(ROOT, "i18n", "*.json"))):
        loc = os.path.basename(f)[:-5]
        for v in json.load(open(f, encoding="utf-8")).values():
            if not isinstance(v, str):
                continue
            for ch in v:
                if ord(ch) > 0x20:      # control chars and space are not glyphs
                    used.setdefault(ord(ch), set()).add(loc)
    return used


def faces():
    out = {}
    for p in sorted(glob.glob(os.path.join(ROOT, "main", "font_kiss_*.c"))):
        out[os.path.basename(p)] = cmap_codepoints(p)
    return out


def selftest(covered):
    """A parser that returned nothing would pass this check on every string,
    so the coverage set is asserted to contain things it must, and to leave
    out something it must not."""
    bad = 0
    for cp, why in ((0x41, "LATIN A"), (0x3042, "HIRAGANA A"),
                    (0xAC00, "HANGUL GA"), (0x4E00, "CJK ONE")):
        if cp not in covered:
            print("SELFTEST: U+%04X (%s) is not in any face, so the cmap "
                  "parser is reading nothing" % (cp, why), file=sys.stderr)
            bad += 1
    # U+E000 opens the private use area. gen_fonts.sh generates nothing there
    # and kiss_theme.h says so, so a parser claiming it is over-reporting.
    if 0xE000 in covered:
        print("SELFTEST: U+E000 is claimed by a face; the parser is "
              "over-reporting ranges", file=sys.stderr)
        bad += 1
    print("text glyph selftest: 5 cases, %d broken" % bad)
    return bad


def main():
    f = faces()
    if not f:
        print("ERROR: no main/font_kiss_*.c found", file=sys.stderr)
        return 1
    covered = set().union(*f.values())
    if selftest(covered):
        print("text glyph gate: refusing to report, the parser is wrong",
              file=sys.stderr)
        return 1

    used = used_codepoints()
    missing = {cp: locs for cp, locs in used.items() if cp not in covered}
    for cp in sorted(missing):
        print("ERROR: U+%04X %r is on screen and in NO font face\n"
              "    -> used in %s\n"
              "       it draws as a blank box, in the simulator too, so no "
              "frame or walk will say so" % (
                  cp, chr(cp), ", ".join(sorted(missing[cp]))), file=sys.stderr)

    print("text glyphs: %d codepoints across %d locales, %d faces, %d missing"
          % (len(used), len({l for ls in used.values() for l in ls}),
             len(f), len(missing)))
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
