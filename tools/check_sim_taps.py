#!/usr/bin/env python3
"""Find scripted taps in the simulator walk that no longer hit anything.

The walk in sim/sim_main.c drives the UI with hardcoded touch coordinates.
When a layout moves, a tap silently lands on empty background: the walk still
passes, the frame still saves, and the frame is simply a copy of the one
before it. Nothing fails. That is how sim_winfo_help quietly stopped capturing
the fingerprint explainer and started capturing the unchanged WALLET screen.

This compares each saved frame with the previous one in walk order. Two
identical frames mean whatever happened between them had no visible effect.

Some pairs are identical on purpose: the walk asserts that a stray tap does
NOT fire a destructive action, or that a shift key STAYS locked. Those are
marked in sim_main.c by their own comments, so they are recognised here rather
than being listed in a second place that would drift.

    bash sim/build_sim.sh && /tmp/fruitsim
    python3 tools/check_sim_taps.py
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIM = os.path.join(ROOT, "sim", "sim_main.c")
# The frames are wherever the walk that made them put them: KISS_SIM_TMP,
# unset it is /tmp, exactly as before (main/kiss_simpath.h). The regexes below
# read save() literals out of the SOURCE, which still say "/tmp/..." because
# that is what the code says -- only the run-time destination moves.
SRC = os.environ.get("KISS_SIM_TMP") or "/tmp"

# A frame whose comment says so is meant to look unchanged. Matching on the
# comment keeps the intent next to the code that relies on it.
DELIBERATE = re.compile(
    r"noop|still the|must be|stays|unchanged|no warning|gone again|nothing loaded",
    re.I)

# A frame saved by a focused harness that runs in its OWN PROCESS, entered by
# an environment variable and never by the ordinary walk. The plain run does
# not write it, so demanding it exist fails the gate on a frame that was never
# due, and it has no neighbour in walk order to be compared against. Its own
# harness asks it more questions than this gate would: the safe-boot one
# checks the title, the cause code and the absence of the game screen.
# Same convention as above -- the intent stays on the line it applies to.
OWN_PROCESS = re.compile(r"own process", re.I)


def parse_saves(lines):
    """[(frame, deliberate_noop)] in the order the walk saves them."""
    out = []
    for line in lines:
        m = re.search(r'save\("/tmp/(sim_[a-z0-9_]+)\.ppm"\)', line)
        if m and not OWN_PROCESS.search(line):
            out.append((m.group(1), bool(DELIBERATE.search(line))))
    return out


def frames():
    with open(SIM) as fh:
        return parse_saves(fh)


# The comparison itself, over data rather than over files, so the self test can
# hand it frames it made up.
def duplicate_pairs(seq):
    """seq: [(name, deliberate, data)] -> [(previous, name)] that are identical."""
    prev_name, prev_bytes = None, None
    dupes = []
    for name, ok_if_same, data in seq:
        if prev_bytes is not None and data == prev_bytes and not ok_if_same:
            dupes.append((prev_name, name))
        prev_name, prev_bytes = name, data
    return dupes


# Half of these must stay QUIET, and that half is the point: this gate is
# defined by its two exemptions -- a frame whose comment says it is meant to
# look unchanged, and a frame saved by a harness in its own process, which the
# plain walk never writes and which has no neighbour to be compared against.
# A version of this check without them would fire on frames nobody can fix,
# and a version that had lost them would be silent on the failure it exists
# for: a tap coordinate left behind by a layout change, landing on empty
# background while the walk stays green and saves the previous screen again.
def selftest():
    bad = 0
    cases = [
        ("two different frames, quiet",
         [("a", False, b"1"), ("b", False, b"2")], 0),
        ("a repeated frame, fires -- the tap did nothing",
         [("a", False, b"1"), ("b", False, b"1")], 1),
        ("a repeated frame the comment calls a noop, quiet",
         [("a", False, b"1"), ("b", True, b"1")], 0),
    ]
    for name, seq, want in cases:
        got = len(duplicate_pairs(seq))
        ok = got == want
        print("  %-52s %s (%d, wanted %d)"
              % (name, "ok" if ok else "FAILED", got, want))
        bad += not ok

    parsed = parse_saves([
        '  save("/tmp/sim_one.ppm");',
        '  save("/tmp/sim_two.ppm");            // stays, deliberately',
        '  save("/tmp/sim_three.ppm");          // its own process',
    ])
    for name, want in [("a plain save() is walked", ("sim_one", False) in parsed),
                       ("a 'stays' comment marks it deliberate",
                        ("sim_two", True) in parsed),
                       ("an 'own process' save is skipped",
                        all(n != "sim_three" for n, _ in parsed))]:
        print("  %-52s %s" % (name, "ok" if want else "FAILED"))
        bad += not want

    print("sim taps selftest: %d cases, %d broken" % (len(cases) + 3, bad))
    return 1 if bad else 0


def main():
    if "--selftest" in sys.argv:
        return selftest()
    seq = frames()
    if not seq:
        sys.stderr.write("no save() calls found in %s\n" % SIM)
        return 1

    paths = [(n, ok, os.path.join(SRC, n + ".ppm")) for n, ok in seq]

    missing = [n for n, _, p in paths if not os.path.exists(p)]
    if missing:
        sys.stderr.write("frames not rendered (run /tmp/fruitsim first):\n  %s\n"
                         % "\n  ".join(missing))
        return 1

    # /tmp accumulates frames from every run anyone has ever done, including
    # the 21-locale walk. Comparing a fresh frame against a leftover one
    # invents differences and hides real ones, so insist they are all from the
    # same run before believing any of it.
    times = {n: os.path.getmtime(p) for n, _, p in paths}
    newest = max(times.values())
    stale = sorted((n for n, t in times.items() if newest - t > 300),
                   key=lambda n: times[n])
    if stale:
        sys.stderr.write(
            "these frames are from an older run than the rest, so the walk\n"
            "did not save them this time. Re-run /tmp/fruitsim:\n  %s\n"
            % "\n  ".join("%s (%.0f min behind)" % (n, (newest - times[n]) / 60)
                          for n in stale[:10]))
        return 1

    loaded = []
    for name, ok_if_same, path in paths:
        with open(path, "rb") as fh:
            loaded.append((name, ok_if_same, fh.read()))
    dupes = duplicate_pairs(loaded)

    print("%d frames checked" % len(seq))
    if not dupes:
        print("ok: every scripted interaction changed the screen")
        return 0

    sys.stderr.write(
        "\n%d frame(s) identical to the one before, so the interaction in\n"
        "between did nothing visible. Usually a tap coordinate left behind by\n"
        "a layout change:\n\n" % len(dupes))
    for a, b in dupes:
        sys.stderr.write("  %-24s == %s\n" % (a, b))
    sys.stderr.write(
        "\nFix the coordinate, or if the frame is meant to look unchanged say\n"
        "so in its comment in sim/sim_main.c (noop / must be / stays / ...).\n")
    return 1


if __name__ == "__main__":
    sys.exit(main())
