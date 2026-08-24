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

The nineteen already here are a BACKLOG, not a pass: they are recorded so the
gate can fail on the twentieth. The list only ever shrinks. When a key on it
comes back into use the run says so and asks for it to be removed, because a
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

# Unreferenced on the day the check landed. Shrink only.
#
# Three groups, and the shape of each says what happened: the SETTINGS page was
# rebuilt around section tabs and left its NET/HISTORY/FW/POP rows behind, and
# STR_R_SP_EXPORT is the value of RECEIVE's SCAN KEY row from when that row was
# a launcher. None of them is deletable in isolation -- deleting a key edits all
# 21 locale files, which is the translation sweep's work, not a wording change's.
BACKLOG = frozenset({
    "STR_G_FW_CH_DARK",
    "STR_G_FW_NEWER",
    "STR_G_FW_OLDER",
    "STR_G_FW_ROW_SIZE",
    "STR_G_FW_ROW_VER",
    "STR_G_HIST_OFF_NOTE",
    "STR_G_HIST_ON_NOTE",
    "STR_G_HIST_ON_NOTE_PLAIN",
    "STR_G_SD_INFO_PILL",
    "STR_I_NET_MAIN_NOTE",
    "STR_I_NET_SIGNET_NOTE",
    "STR_I_NET_TEST_NOTE",
    "STR_I_POP_CHIP",
    "STR_I_POP_ERASE",
    "STR_I_POP_KEEP",
    "STR_I_ROW_HISTORY_SUB",
    "STR_I_SEC_HISTORY",
    "STR_R_SP_EXPORT",
    "STR_R_USAGE_UNKNOWN",
    # KEYS re-weighted to the identity-as-headline shape: the fingerprint
    # became the hero and its old row sub went with the row. Translated in 21
    # locales, so it waits for the sweep like the rest of this list.
    "STR_K_FP_SUB",
    # The scan key gate: its hold said the title over again in a lane the
    # words could not fit, so it shares HOLD TO SHOW with the word grid.
    "STR_R_SP_SHOW",
    # Subtitles retired by the trails pass: every opened-from page now names
    # its path at y=70 instead of restating its title, its content or a value
    # the screen already shows. Each waits for the sweep like the rest.
    "STR_G_STORAGE_CURRENT_FMT",
    "STR_I_KEF_SHOW_S",
    "STR_I_KEF_WARN_S_PP",
    "STR_I_PAIR_S",
    "STR_I_ROW_WAYSIN_SUB",
    "STR_R_SP_EXPORT_S",
    "STR_W_AUD_S",
    "STR_W_MADE_S",
})

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
