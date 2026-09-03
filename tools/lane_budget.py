#!/usr/bin/env python3
"""What must this locale's copy be cut TO, key by key.

The overlap gate says a string is too wide. It says so about the STRING as it
was rendered -- truncated, joined across hand-set line breaks, sometimes with an
ellipsis already in it -- and it names a pixel lane rather than a key. So every
locale pass so far has started with the same half hour of hand work: read the
findings, work out which i18n key each one is, divide the width by the character
count to get this script's pixels per character, and turn the lane into a budget
you can actually write to.

That is arithmetic, and it is the same arithmetic every time. Three locales were
cut by hand before it was written down; there are seventeen left.

    python3 tools/lane_budget.py <findings-file> <locale>

Reads a saved run of sim/run_overlapcheck.sh and prints one row per key:

    KEY                 kind    now  ->  keep   lane   en / locale value

`keep` is what the copy has to fit in. It is derived per finding, from that
finding's own width and length, so it carries the locale's real glyph width
rather than an assumption about Latin or Cyrillic.

Rows it cannot name print KEY as "?" with the rendered text, which means the
string is built at runtime (a format string with its %s filled in, or two keys
concatenated) and has to be found by hand. That is the only part of this that
was ever interesting.
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]

CUT = re.compile(
    r'CUT\s+(fact value|sub-line|row label|fact caption) "(.+?)" '
    r'wants (\d+)px of a (\d+)px lane')
CLIPX = re.compile(
    r'CLIPX\s+"(.+?)" asks for x (-?\d+)\.\.(-?\d+), visible only (-?\d+)\.\.(-?\d+)')
DOTS = re.compile(r'DOTS\s+"(.+?)" is wearing an ellipsis')


def load(loc):
    en = json.loads((ROOT / "i18n" / "en.json").read_text(encoding="utf-8"))
    tr = json.loads((ROOT / "i18n" / f"{loc}.json").read_text(encoding="utf-8"))
    return en, tr


def index(tr):
    """Every way a key's value can reach the glass, mapped back to the key."""
    idx = {}
    for k, v in tr.items():
        for form in (v, v.replace("\n", " "), v.split("\n")[0],
                     v.split("\n\n")[0].replace("\n", " ")):
            idx.setdefault(form, k)
    return idx


def keyfor(s, idx, tr):
    if s in idx:
        return idx[s]
    # the gate truncates with an ellipsis; match on what is left of the front
    core = s.rstrip(".").rstrip("…").strip()
    if len(core) >= 8:
        hits = [k for k, v in tr.items()
                if v.startswith(core) or v.replace("\n", " ").startswith(core)]
        if len(hits) == 1:
            return hits[0]
    return "?"


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    path, loc = sys.argv[1], sys.argv[2]
    en, tr = load(loc)
    idx = index(tr)
    rows, seen = [], set()
    # A 21 locale sweep is one file with every locale's findings in it, under a
    # "<locale>  N findings" header each. Reading the whole file would hand a
    # French budget a German string, so the section is tracked and everything
    # outside this locale's is skipped. A single locale run has one section and
    # falls through the same code.
    section = None
    head = re.compile(r'^([a-z]{2}(?:-[A-Z]{2})?)\s+(?:\d+ findings|clean|NO SUMMARY)')
    for line in open(path, encoding="utf-8", errors="replace"):
        h = head.match(line)
        if h:
            section = h.group(1)
            continue
        if section is not None and section != loc:
            continue
        m = CUT.search(line)
        if m:
            kind, s, want, lane = m.group(1), m.group(2), int(m.group(3)), int(m.group(4))
        else:
            m = CLIPX.search(line)
            if m:
                s = m.group(1)
                want = int(m.group(3)) - int(m.group(2))
                lane = int(m.group(5)) - int(m.group(4))
                kind = "clipped"
            else:
                continue
        if s in seen:
            continue
        seen.add(s)
        per = want / max(len(s), 1)
        rows.append((keyfor(s, idx, tr), kind, len(s), int(lane / per), lane, s))

    rows.sort(key=lambda r: (r[0] == "?", r[3] - r[2]))
    print(f"{loc}: {len(rows)} strings over their lane\n")
    for k, kind, now, keep, lane, s in rows:
        over = now - keep
        print(f"{k:24} {kind:11} {now:>3}ch -> keep {keep:>3}ch "
              f"(cut {over:>2}, lane {lane}px)")
        if k != "?":
            print(f"    en: {en.get(k)!r}")
        print(f"    {loc}: {s!r}")
    unnamed = sum(1 for r in rows if r[0] == "?")
    if unnamed:
        print(f"\n{unnamed} built at runtime, name them by hand")
    return 0


if __name__ == "__main__":
    sys.exit(main())
