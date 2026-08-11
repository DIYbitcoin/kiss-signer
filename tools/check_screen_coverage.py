#!/usr/bin/env python3
"""Which screens has no gate ever looked at?

overlapcheck asks seven questions per STOP. A screen with no stop is a screen
nobody has ever asked about -- and that is not hypothetical: whatseed_open was a
title, a subtitle and one 704x232 paragraph, BARE by rule 1, on the screen a
newcomer opens to find out what a seed is. Every gate reported clean for its
entire life because no walk stop rendered it. It was found by accident.

Two halves, because one alone is blind:

  built but never captured   the walk opens the screen and never photographs it
                             (reported by sim_main itself, see wt_sim_uncaptured)
  never built at all         the walk never opens it, so the check above cannot
                             see it either -- this script's job

The second half needs the set of screens that EXIST, which only the source
knows. Every screen is wt_screen(parent, title, sub) or one of the mk_screen
wrappers, and the title is nearly always tr(STR_KEY), so the keys are greppable.
Titles built from a variable are counted as unresolvable and printed, because a
gate that quietly ignores what it cannot parse is the thing being fixed.

    python3 tools/check_screen_coverage.py          report
    SCREENCOVER_STRICT=1 python3 tools/...          exit 1 on any uncovered

Run after sim/build_sim.sh; this drives /tmp/fruitsim itself.
"""
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SIM = Path("/tmp/fruitsim")

# wt_screen(parent, title, ...) / mk_screen(title, ...) / mk_screen2(title, ...)
# Capture the first argument for the mk_ forms and the second for wt_screen,
# then take every STR_ token in it: a ternary picks between two real titles and
# both of them are screens somebody can reach.
CALLS = [
    (re.compile(r"\bmk_screen2?\s*\(([^;]*?)\)\s*;", re.S), 0),
    (re.compile(r"\bwt_screen\s*\(([^;]*?)\)\s*;", re.S), 1),
]
KEY = re.compile(r"\bSTR_([A-Z0-9_]+)\b")


def split_args(text):
    """Top level comma split, so tr(STR_X) stays in one piece."""
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    out.append(cur)
    return out


def titles_in_source():
    """{key: [files]} for every screen title, plus a list of unresolvable sites."""
    found, murky = {}, []
    for path in sorted((ROOT / "main").glob("*.c")):
        if path.name in ("kiss_theme.c", "i18n_tables.c"):
            continue          # the definition itself, and the generated table
        src = path.read_text(encoding="utf-8", errors="replace")
        for pattern, argno in CALLS:
            for m in pattern.finditer(src):
                args = split_args(m.group(1))
                if len(args) <= argno:
                    continue
                keys = KEY.findall(args[argno])
                if not keys:
                    line = src[: m.start()].count("\n") + 1
                    murky.append(f"{path.relative_to(ROOT)}:{line}")
                    continue
                for k in keys:
                    found.setdefault(k, []).append(path.name)
    return found, murky


def titles_built():
    """English titles the walk actually opened, from the sim itself."""
    if not SIM.exists():
        sys.exit(f"{SIM} is missing: run bash sim/build_sim.sh first")
    env = dict(os.environ, SCREENCOVER_LIST="1")
    run = subprocess.run([str(SIM)], capture_output=True, text=True, env=env)
    return {l.split("\t", 1)[1] for l in run.stdout.splitlines() if l.startswith("BUILT\t")}


def selftest():
    """Prove the 'built but never captured' half can still fire.

    Same rule run_overlapcheck.sh applies to WALL and ROLE: a count of zero
    means nothing unless the check still reports something when it should. The
    sim builds a screen titled OK and never saves it; that must come back.
    """
    env = dict(os.environ, SCREENCOVER_SELFTEST="1")
    run = subprocess.run([str(SIM)], capture_output=True, text=True, env=env)
    return any(l.startswith("  UNCHECKED") and '"OK"' in l for l in run.stdout.splitlines())


def main():
    if not selftest():
        print("FAILED: the coverage self test no longer reports, so a clean run "
              "means nothing.")
        return 1

    found, murky = titles_in_source()
    built = titles_built()

    # The source gives keys, the sim gives English strings, so meet in the
    # middle through the same table the firmware reads.
    # FIRST occurrence only. The generated file holds 21 tables one after
    # another and every key appears in all of them, so dict() over the whole
    # file silently hands back Croatian.
    tables = (ROOT / "main" / "i18n_tables.c").read_text(encoding="utf-8")
    en = {}
    for key, text in re.findall(r'\[STR_([A-Z0-9_]+)\]\s*=\s*"((?:[^"\\]|\\.)*)"', tables):
        en.setdefault(key, text)

    missing = []
    for key in sorted(found):
        text = en.get(key)
        if text is None:
            continue                      # not a translated title; nothing to match
        if text.encode().decode("unicode_escape") not in built and text not in built:
            missing.append((key, text, sorted(set(found[key]))))

    print(f"screen coverage: {len(found)} titles in source, {len(built)} opened by the walk")
    for key, text, files in missing:
        print(f'  NEVER OPENED  STR_{key}  "{text}"  ({", ".join(files)})')
    if murky:
        print(f"  {len(murky)} title(s) built from a variable, not checkable here:")
        for site in murky:
            print(f"    {site}")
    print(f"screen coverage: {len(missing)} screens the walk never opens")

    if missing and os.environ.get("SCREENCOVER_STRICT"):
        print("\nFAILED: SCREENCOVER_STRICT is set and a screen has no walk stop.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
