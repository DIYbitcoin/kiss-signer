#!/usr/bin/env python3
"""Find icon codepoints the firmware draws that the fonts do not contain.

A glyph that is not in the generated fonts does not fail, and it does not
look like a failure. LVGL draws a blank box about half a line wide, and it
draws exactly the same box in the simulator as on the panel -- so a wrong
codepoint survives every gate this project has, ships, and is found by
somebody holding the device. main/kiss_theme.h says so twice, above
wt_row_x and above wt_tabs, in the same words: "a wrong pick survives every
gate and is caught on glass."

This is that gate. It reads the two lists that already exist rather than
keeping a third that would drift from both:

  tools/fonts/gen_fonts.sh   SYMS, the codepoints handed to lv_font_conv
  lvgl .../lv_symbol_def.h   what each LV_SYMBOL_* name resolves to

and compares them against every icon the firmware actually names -- both
the LV_SYMBOL_* constants and the raw UTF-8 escapes the house icons are
written as (WT_ICON_SHIELD is "\\xEF\\x8F\\xAD", U+F3ED).

    python3 tools/check_glyphs.py

Exit 1 on a missing glyph. Unused SYMS entries are reported but do not
fail: a codepoint kept for a screen that has not been written yet costs
font bytes and nothing else, and deciding that is a person's job.

Like tools/check_screen_coverage.py it SELF TESTS first, because a checker
that has quietly stopped checking reports a clean sweep either way. The
env var only makes it SAY so; the scan runs either way:

    GLYPHCHECK_SELFTEST=1 python3 tools/check_glyphs.py
"""

import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN_FONTS = os.path.join(ROOT, "tools", "fonts", "gen_fonts.sh")
SYMBOL_DEF = os.path.join(ROOT, "managed_components", "lvgl__lvgl", "src",
                          "font", "lv_symbol_def.h")

# The generated font tables are megabytes of glyph data that mention every
# codepoint by construction, and i18n_tables.c is translated copy rather
# than icons. Scanning either would report the whole set as "used".
SKIP = re.compile(r"font_kiss|i18n_tables|flag_imgs|_img\.c$|sprites\.c$")

# Anything below this is text, not an icon. FontAwesome's private use area
# starts at U+E000; the escapes we care about are all U+F0xx and up.
ICON_FLOOR = 0xE000


def strip_comments(src):
    """Comments name glyphs they do not draw -- kiss_theme.h documents every
    WT_ICON_* with its codepoint in the line above it. Counting those as uses
    would demand a font for a glyph nothing renders."""
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", " ", src)


def syms():
    """The codepoints gen_fonts.sh hands to lv_font_conv."""
    with open(GEN_FONTS) as fh:
        for line in fh:
            m = re.match(r'SYMS="([\d,]+)"\s*$', line.strip())
            if m:
                return {int(x) for x in m.group(1).split(",")}
    sys.stderr.write("no SYMS= line in %s\n" % GEN_FONTS)
    sys.exit(2)


def lv_symbols():
    """LV_SYMBOL_* -> codepoint, from LVGL's own header and never a copy."""
    out = {}
    with open(SYMBOL_DEF) as fh:
        for line in fh:
            m = re.match(r'\s*#define\s+(LV_SYMBOL_\w+)\s+"((?:\\x[0-9A-Fa-f]{2})+)"',
                         line)
            if not m:
                continue
            raw = bytes(int(x, 16)
                        for x in re.findall(r"\\x([0-9A-Fa-f]{2})", m.group(2)))
            try:
                out[m.group(1)] = ord(raw.decode("utf-8"))
            except (UnicodeDecodeError, TypeError):
                pass          # the multi-glyph aliases; nothing draws them alone
    if not out:
        sys.stderr.write("no LV_SYMBOL_* found in %s\n" % SYMBOL_DEF)
        sys.exit(2)
    return out


def used(extra_source=None):
    """{codepoint: {file, ...}} for every icon the firmware names."""
    names = lv_symbols()
    out = {}

    def scan(src, where):
        src = strip_comments(src)
        for m in re.finditer(r'"((?:\\x[0-9A-Fa-f]{2}){2,4})"', src):
            raw = bytes(int(x, 16)
                        for x in re.findall(r"\\x([0-9A-Fa-f]{2})", m.group(1)))
            try:
                cp = ord(raw.decode("utf-8"))
            except (UnicodeDecodeError, TypeError):
                continue
            if cp >= ICON_FLOOR:
                out.setdefault(cp, set()).add(where)
        for name, cp in names.items():
            if re.search(r"\b%s\b" % name, src):
                out.setdefault(cp, set()).add(where)

    for path in sorted(glob.glob(os.path.join(ROOT, "main", "*.c")) +
                       glob.glob(os.path.join(ROOT, "main", "*.h"))):
        if SKIP.search(os.path.basename(path)):
            continue
        with open(path, encoding="utf-8", errors="replace") as fh:
            scan(fh.read(), os.path.basename(path))

    if extra_source:
        scan(extra_source, "<selftest>")
    return out


def report(have, want, quiet=False):
    missing = {cp: f for cp, f in want.items() if cp not in have}
    if not quiet:
        for cp, files in sorted(missing.items()):
            print("MISSING U+%04X  named in %s  -- draws a blank box, everywhere"
                  % (cp, ", ".join(sorted(files))))
    return missing


def main():
    have = syms()

    # A glyph nothing has ever asked for, injected into a scan of the real
    # tree. If this does not come back the checker is not checking, and a
    # clean sweep below would mean nothing. U+F5FC is not in SYMS and is not
    # named anywhere in main/.
    probe = 0xF5FC
    if probe in have:
        sys.stderr.write("self test picked a codepoint that IS in SYMS; "
                         "choose another\n")
        return 2
    seeded = used(extra_source='static const char *probe = "\\xEF\\x97\\xBC";')
    if probe not in seeded or probe in syms():
        sys.stderr.write("SELF TEST FAILED: the check no longer fires. "
                         "Fix it before trusting a clean run.\n")
        return 2
    # And the reporting half, which is the part that can rot silently: a
    # scanner that finds the probe is worth nothing if report() has stopped
    # calling it missing. Checked without printing, so a real run says only
    # what it found in the real tree.
    if not report({}, {probe: {"<selftest>"}}, quiet=True):
        sys.stderr.write("SELF TEST FAILED: an absent codepoint was not "
                         "reported. Fix it before trusting a clean run.\n")
        return 2
    if os.environ.get("GLYPHCHECK_SELFTEST"):
        # ...and then keep going, which it did not. This returned 0 here, and
        # the invocation CLAUDE.md's gate list prescribes is exactly
        # GLYPHCHECK_SELFTEST=1 -- so the one command anybody runs proved the
        # self test worked and never looked at the tree. check_gates.py and
        # check_screen_coverage.py both print the self test and continue; this
        # is the same shape, and the flag now means "say the self test passed"
        # rather than "stop after it".
        print("self test ok: an absent codepoint is reported")

    want = used()
    missing = report(have, want)
    spare = sorted(cp for cp in have if cp not in want)

    print("glyph gate: %d codepoints in SYMS, %d named by the firmware, "
          "%d missing" % (len(have), len(want), len(missing)))
    if spare:
        # Not a failure. A codepoint kept for a screen not yet written costs
        # font bytes in four scripts and nothing else.
        print("glyph gate: %d in SYMS that nothing names (%s)"
              % (len(spare), ", ".join("U+%04X" % c for c in spare)))
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
