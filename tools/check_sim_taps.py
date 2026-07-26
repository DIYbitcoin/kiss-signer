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
SRC = "/tmp"

# A frame whose comment says so is meant to look unchanged. Matching on the
# comment keeps the intent next to the code that relies on it.
DELIBERATE = re.compile(
    r"noop|still the|must be|stays|unchanged|no warning|gone again|nothing loaded",
    re.I)


def frames():
    """[(frame, deliberate_noop)] in the order the walk saves them."""
    out = []
    with open(SIM) as fh:
        for line in fh:
            m = re.search(r'save\("/tmp/(sim_[a-z0-9_]+)\.ppm"\)', line)
            if m:
                out.append((m.group(1), bool(DELIBERATE.search(line))))
    return out


def main():
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

    prev_name, prev_bytes = None, None
    dupes = []
    for name, ok_if_same, path in paths:
        with open(path, "rb") as fh:
            data = fh.read()
        if prev_bytes is not None and data == prev_bytes and not ok_if_same:
            dupes.append((prev_name, name))
        prev_name, prev_bytes = name, data

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
