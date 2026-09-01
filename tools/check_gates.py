#!/usr/bin/env python3
"""Every checker in tools/ is run by something.

This is the gate for the gates, and it exists because two were not. It is the
smallest check in the repo and the only one whose subject is the repo's own
habits rather than the product.

`check_glyphs.py` and `check_mono_glyphs.py` were written, they self test, they
pass -- and no workflow, no build script and no line of CLAUDE.md invoked
either. They had been that way long enough that nobody could say when it
started. What they cover is the blind spot `main/kiss_theme.h` names twice in
its own words, *a wrong pick survives every gate and is caught on glass*: a
codepoint missing from the generated fonts draws a blank box about half a line
wide, and draws it IDENTICALLY in the simulator, so no frame, no walk and no
overlap check has ever had an opinion about it.

Nothing was broken. That is the point -- a gate nothing runs is not a gate that
is failing, it is a gate that is absent, and absent looks exactly like green.

**Being LISTED is not the test. Being RUN is.** Four checkers are deliberately
absent from CLAUDE.md's gate block and all four are covered:
`check_cur_link.py` runs inside `sim/build_test.sh`, `check_flash_budget.py`
and `check_fw_version.py` inside the release scripts, `check_sim_taps.py` in
CI. Adding them to a list a person reads would make the list longer and the
repo no safer, so this asks the question that matters and not the tidy one.

    python3 tools/check_gates.py
    GATECHECK_SELFTEST=1 python3 tools/check_gates.py
"""
import glob
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Where a checker can be invoked from. CLAUDE.md counts: a command a person is
# told to run is run, and that is the whole lane for the ones the owner drives
# by hand between commits.
RUNNERS = (".github/workflows/*.yml", "tools/*.sh", "sim/*.sh", "tools/*.py",
           "CLAUDE.md", "HOUSE-RULES.md", "CONTRIBUTING.md")

# A checker that only ever names ITSELF is not invoked -- its own docstring and
# its own argv do not count, which is the case that made this necessary.
SKIP_SELF = True


def runners():
    """Every file that could invoke a checker, as (path, text)."""
    out = []
    for pat in RUNNERS:
        for p in sorted(glob.glob(os.path.join(ROOT, pat))):
            try:
                out.append((os.path.relpath(p, ROOT),
                            open(p, encoding="utf-8", errors="ignore").read()))
            except OSError:
                pass
    return out


def orphans(checkers, files):
    """Checkers no file names. Returns {checker: []} for the ones nothing runs."""
    bad = {}
    for c in checkers:
        where = [rel for rel, txt in files
                 if not (SKIP_SELF and rel == "tools/" + c) and c in txt]
        if not where:
            bad[c] = []
    return bad


def main():
    files = runners()
    checkers = sorted(os.path.basename(p)
                      for p in glob.glob(os.path.join(ROOT, "tools", "check_*.py")))

    if os.environ.get("GATECHECK_SELFTEST"):
        broken = []
        # Must fire: a checker nothing names. Assembled at runtime rather than
        # written out, because THIS FILE is one of the files being searched --
        # spelling the synthetic name here would put it in the haystack and the
        # case would pass by finding itself. It did, on the first run.
        ghost = "check_" + "nothing_runs" + "_this.py"
        if ghost not in orphans([ghost], files):
            broken.append("an unreferenced checker was NOT reported")
        # Must stay quiet: one that is genuinely invoked. Reading it out of the
        # tree rather than hard-coding a name, so deleting that checker retires
        # the case instead of failing it for the wrong reason.
        covered = [c for c in checkers if c not in orphans(checkers, files)]
        if not covered:
            broken.append("no checker is invoked anywhere, which cannot be right")
        elif orphans([covered[0]], files):
            broken.append(f"{covered[0]} is invoked and WAS reported")
        # Must not be satisfied by a checker naming only itself.
        if not orphans(["check_gates.py"],
                       [("tools/check_gates.py", "check_gates.py")]):
            broken.append("a checker naming only itself counted as invoked")
        if broken:
            for b in broken:
                print("gate selftest:", b)
            return 1
        print("gate selftest: 3 cases, 0 broken")

    bad = orphans(checkers, files)
    print(f"gate coverage: {len(checkers)} checkers, "
          f"{len(checkers) - len(bad)} invoked, {len(bad)} run by nothing")
    for c in sorted(bad):
        print(f"  tools/{c} is invoked by no workflow, no script and no rule "
              f"file -- wire it in or delete it")
    return 1 if bad else 0


sys.exit(main())
