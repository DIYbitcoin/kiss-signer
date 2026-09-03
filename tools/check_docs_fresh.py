#!/usr/bin/env python3
"""How far behind the screens the documentation pictures have fallen.

docs/shots, docs/readme, docs/media and both docs/review axes are frames the
simulator rendered, so they are only as current as the last run of
tools/gen_docs_shots.sh. Nothing regenerates them on a commit, and nothing
noticed when they stopped matching: they sat at Aug 30 while develop changed
main/ twenty one times, and the KEYS page in the walkthrough was a page the
device no longer draws.

This does NOT compare pixels, for the reason gen_docs_shots.py gives about its
own --check: two zlib versions encode the same image differently and a flaky
docs job teaches people to ignore the docs job. It compares COMMITS. The last
commit that touched a rendered screen, against the last commit that touched
the pictures. The gap between them is the number that matters, and it is
exact.

Deliberately two speeds, because a regeneration is a simulator build plus
forty two binaries and does not belong in every UI commit:

    python3 tools/check_docs_fresh.py            # report the gap, exit 0
    python3 tools/check_docs_fresh.py --strict   # a gap is a failure

Plain on the way into develop, where a gap is normal and the count is just
something to read. --strict on the way to main, where the gap is what ships.

    python3 tools/check_docs_fresh.py --selftest # prove the counter counts
"""

import subprocess
import sys

# A screen is drawn from main/ and posed by the walk in sim/sim_main.c. The
# rest of sim/ is gates and harness -- it cannot change what a frame looks
# like, so it is not a reason to re-render one.
UI = ["main/", "sim/sim_main.c"]
# Written by gen_docs_shots.py: the commit the frames were rendered from.
STAMP = "docs/shots/.rendered"
# Fallback when the stamp is absent (a clone from before it existed): the four
# directories a regeneration writes, so whichever moved dates the run.
PICTURES = ["docs/shots/", "docs/readme/", "docs/media/", "docs/review/"]


def git(*args):
    r = subprocess.run(["git", *args], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else ""


def commits_since(base, paths):
    """UI commits newer than base, oldest last, as (sha, subject)."""
    out = git("log", "--format=%h\t%s", f"{base}..HEAD", "--", *paths)
    return [tuple(l.split("\t", 1)) for l in out.splitlines() if "\t" in l]


def rendered_from():
    """(commit, how it was dated) for the last regeneration.

    The stamp is the accurate answer: a run that changes no pixel still writes
    it, so closing the gap is always recordable. A commit that only renames a
    save() literal cannot move a picture, and without the stamp that rename
    reads as a screen change with no regeneration behind it -- which is a gate
    crying wolf on the one repo where the frames are already correct.
    """
    sha = git("show", f"HEAD:{STAMP}").strip()
    if sha:
        when = git("log", "-1", "--format=%h %ci", sha)
        if when:
            return sha, when + "  (stamped)"
    last = git("log", "-1", "--format=%h %ci", "--", *PICTURES)
    if not last:
        return "", ""
    return last.split()[0], last + "  (dated by the pictures; no stamp)"


def selftest():
    """A check nobody has seen fire is a check nobody should trust."""
    if not git("log", "-1", "--format=%H", "--", *PICTURES):
        sys.exit("selftest: no commit has ever touched the picture directories")
    # Must stop: from HEAD there is nothing newer than HEAD.
    if commits_since("HEAD", UI):
        sys.exit("selftest: counted UI commits newer than HEAD")
    # Must fire: from the parent of the newest UI commit, that commit itself
    # is in range, so an empty answer means the counter is looking elsewhere.
    ui = git("log", "-1", "--format=%H", "--", *UI)
    if not ui:
        sys.exit("selftest: no commit has ever touched a screen")
    if not commits_since(f"{ui}^", UI):
        sys.exit("selftest: the counter did not fire on a known UI commit")
    print("selftest: the counter fires on a UI commit, and stops at HEAD")


def main():
    strict = "--strict" in sys.argv
    if "--selftest" in sys.argv:
        return selftest()

    if git("rev-parse", "--is-shallow-repository") == "true":
        msg = "shallow clone: the gap cannot be measured (needs fetch-depth: 0)"
        if strict:
            sys.exit("docs freshness: " + msg)
        print("docs freshness: " + msg)
        return

    base, last = rendered_from()
    if not base:
        sys.exit("docs freshness: nothing records when the frames were rendered")

    behind = commits_since(base, UI)
    if not behind:
        print(f"docs freshness: pictures are current (last rendered at {last})")
        return

    print(f"docs freshness: pictures rendered at {last}")
    print(f"  {len(behind)} commits have changed a screen since:")
    for sha, subject in behind[:10]:
        print(f"    {sha}  {subject}")
    if len(behind) > 10:
        print(f"    ... and {len(behind) - 10} more")
    print("  regenerate with: bash tools/gen_docs_shots.sh")
    if strict:
        sys.exit("docs freshness: stale pictures must not ship")


if __name__ == "__main__":
    main()
