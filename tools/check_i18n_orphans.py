#!/usr/bin/env python3
"""Which translated strings does nothing on the device use?

Every key in i18n/en.json is carried in twenty other locales and compiled into
main/i18n_tables.c for all of them. A key nobody references is therefore paid
21 times, in flash, for ever, and nothing in the tree noticed: a screen gets
rebuilt, its old strings stop being called, and the keys sit there translated.
32 of them were deleted in one sweep once, by hand, after somebody happened to
look.

So this is the check that looks. It reads the enum in main/i18n_keys.h -- which
gen_i18n.py writes from en.json, so it is the full set -- and greps every C and
Python source that could name one. What is left over is unreferenced.

The backlog is EMPTY, and that is the state to keep it in: it held 55 keys,
recorded so the gate could fail on the fifty-sixth, and they were deleted in
one sweep across all 21 locale files. Anything that lands on it again is a
screen that was rebuilt and left its old strings behind. The list only ever
shrinks, and a key on it that comes back into use is reported, because a
backlog nobody prunes is a list of lies.

    python3 tools/check_i18n_orphans.py        report, and fail on anything new

The check self tests before it reports: a key that cannot possibly be
referenced is added to the set and must come back flagged. A gate that has
quietly stopped firing reports a clean sweep exactly like a clean tree does.
"""
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Where a key can legitimately be named. i18n_keys.h IS the enum and
# i18n_tables.c is the generated value table, so neither counts as a use --
# and neither does THIS FILE, which names nineteen keys in its backlog and
# would otherwise report every one of them as back in use, itself.
SEARCH_DIRS = ("main", "sim", "tools")
SEARCH_EXT = (".c", ".h", ".cpp", ".py")
GENERATED = ("i18n_keys.h", "i18n_tables.c")
SELF = Path(__file__).resolve()

KEY_RE = re.compile(r"\bSTR_[A-Z0-9_]+\b")
ENUM_RE = re.compile(r"^\s*(STR_[A-Z0-9_]+)\s*,\s*$")

# Empty, and meant to stay that way. Shrink only.
#
# It carried 55 keys, in three groups whose shape said what had happened: the
# SETTINGS page rebuilt around section tabs, leaving its NET/HISTORY/FW/POP rows
# behind; the sign flow's old COORDINATOR explainer; and STR_R_SP_EXPORT, the
# value of RECEIVE's SCAN KEY row from when that row was a launcher.
#
# This used to say they were not deletable in isolation, because deleting a key
# edits all 21 locale files and that is the translation sweep's work. It is not:
# a DELETION carries no wording anywhere, so nothing about it can be thrown away
# when the sweep happens, and holding 55 dead keys in flash in 21 languages to
# wait for a pass that changes none of them was paying for nothing. One reviewer
# also read STR_R_ONE_EACH off en.json and filed a finding about a string the
# device has never drawn, which is the other cost.
BACKLOG = frozenset()

# The self test's needle: a key nothing can reference, and nothing does --
# including this line, because the scan skips this file.
SELFTEST_KEY = "STR_ZZ_SELFTEST_NEVER_REFERENCED"


def enum_keys():
    """Every key the generator wrote, in enum order."""
    header = ROOT / "main" / "i18n_keys.h"
    keys = []
    for line in header.read_text(encoding="utf-8").splitlines():
        m = ENUM_RE.match(line)
        if m and not m.group(1).startswith("STR_COUNT"):
            keys.append(m.group(1))
    if not keys:
        sys.exit(f"FAILED: no STR_ keys parsed out of {header} -- has the "
                 "generator changed shape?")
    return keys


def referenced():
    """Every key named by something that is not the generated pair."""
    names = set()
    for d in SEARCH_DIRS:
        for path in sorted((ROOT / d).rglob("*")):
            if not path.is_file() or path.suffix not in SEARCH_EXT:
                continue
            if path.name in GENERATED or path.resolve() == SELF:
                continue
            names |= set(KEY_RE.findall(
                path.read_text(encoding="utf-8", errors="ignore")))
    return names


def orphans_of(keys, used):
    return [k for k in keys if k not in used]


def selftest(used):
    """A key nothing can reference must come back flagged."""
    found = orphans_of([SELFTEST_KEY], used)
    if found != [SELFTEST_KEY]:
        sys.exit("FAILED: self test did not fire -- the orphan check no longer "
                 "detects an unreferenced key, so a clean report means nothing")


def main():
    keys = enum_keys()
    used = referenced()
    selftest(used)

    found = set(orphans_of(keys, used))
    new = sorted(found - BACKLOG)
    fixed = sorted(BACKLOG - found)

    print(f"i18n orphans: {len(keys)} keys, {len(found)} unreferenced "
          f"({len(BACKLOG)} on the backlog)")

    for k in fixed:
        print(f"  BACKLOG STALE  {k} is referenced again -- remove it from "
              "BACKLOG in this file")
    for k in new:
        print(f"  NEW ORPHAN     {k} is translated in 21 locales and nothing "
              "calls it")

    if new:
        print(f"i18n orphans: {len(new)} new. Either use the key or delete it "
              "from i18n/*.json at the translation sweep and regenerate.")
        return 1
    if fixed:
        return 1
    print("i18n orphans: no new ones")
    return 0


if __name__ == "__main__":
    sys.exit(main())
