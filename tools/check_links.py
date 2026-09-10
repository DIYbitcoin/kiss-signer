#!/usr/bin/env python3
"""Read every relative link in the tracked Markdown and refuse one that
points at a file that is not there.

    check_links.py            # check the tree
    check_links.py --selftest

WHY THIS EXISTS. `docs/` is what GitHub Pages serves, so a link written in
one of those files is a link a reader clicks on the published site. Nothing
ever resolved them. Six were broken at once, and four of the six were the
same mistake in the same file: `docs/ROADMAP.md` linking to
`docs/security-plan.md`, which is the path from the repository root and not
from the file doing the linking. On the site that is a 404 on the roadmap's
own link to the security plan -- the two documents a careful reader goes
looking for first, and the pair most likely to be read by someone deciding
whether to trust the firmware with their coins.

The mistake is easy to make and invisible to every other gate: the link
looks right in the editor, it looks right in a review diff, and it even
resolves on github.com when the file is browsed from the repository root
instead of from Pages.

WHAT IT CHECKS. Every inline `[text](target)` and every reference
definition `[label]: target` in tracked `.md` files. A target is resolved
against the directory of the file that holds it. http, https, mailto and
tel are left alone, and so is a bare `#anchor`. Vendored upstream trees are
skipped: their links are their project's business and several are broken in
the copies we carry.

It reports how many files it read as well as how many links it rejected,
because a scan that finds nothing and a scan that reads nothing print the
same word.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

INLINE = re.compile(r"\[[^\]]*\]\(\s*<?([^)>\s]+)")
REFDEF = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*<?(\S+)>?\s*$", re.M)
SKIP_SCHEME = ("http://", "https://", "mailto:", "tel:", "ftp://", "data:")
# Vendored trees. Their broken links are upstream's, not ours.
VENDORED = ("components/libwally-core/upstream/", "managed_components/")


def targets(text):
    """Every link target in one file, inline and reference style."""
    return [m.group(1) for m in INLINE.finditer(text)] + REFDEF.findall(text)


def scan(root, files):
    """Return a list of (file, target) that do not resolve."""
    bad = []
    for rel in files:
        path = os.path.join(root, rel)
        try:
            text = open(path, encoding="utf-8", errors="replace").read()
        except OSError as e:
            bad.append((rel, f"<unreadable: {e}>"))
            continue
        for raw in targets(text):
            target = raw.split("#", 1)[0].strip()
            if not target or target.startswith(SKIP_SCHEME):
                continue
            here = os.path.dirname(rel)
            resolved = os.path.normpath(os.path.join(root, here, target))
            if not os.path.exists(resolved):
                bad.append((rel, raw))
    return bad


def tracked_markdown(root):
    out = subprocess.run(
        ["git", "-C", root, "ls-files", "*.md", "*.markdown"],
        capture_output=True, text=True, check=True).stdout.split()
    return [f for f in out if not f.startswith(VENDORED)]


def check(root):
    files = tracked_markdown(root)
    # The trap this repository has been caught by before: a gate that looked
    # at nothing reported clean for its whole life. No files is a failure.
    if not files:
        print("::error::no tracked Markdown found -- this gate read nothing")
        return 1
    bad = scan(root, files)
    print(f"read {len(files)} tracked Markdown file(s)")
    print(f"broken relative links: {len(bad)}")
    for f, t in bad:
        print(f"  {f} -> {t}")
    if bad:
        print("::error::a tracked Markdown file links to a path that is not "
              "there. A link resolves from the file holding it, not from the "
              "repository root.")
        return 1
    return 0


def selftest():
    """Plant the exact defect this exists for and prove the scan catches it."""
    fails = 0
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, "docs", "specs"))
        open(os.path.join(d, "docs", "security-plan.md"), "w").close()
        open(os.path.join(d, "docs", "specs", "a.md"), "w").close()
        open(os.path.join(d, "CHANGELOG.md"), "w").close()

        # The real mistake: a root-relative path used from inside docs/.
        planted = os.path.join(d, "docs", "ROADMAP.md")
        open(planted, "w").write(
            "See [the plan](docs/security-plan.md) and [gone](nope.md).\n")
        bad = scan(d, ["docs/ROADMAP.md"])
        if len(bad) != 2:
            print(f"::error::planted 2 broken links, scan found {len(bad)}")
            fails += 1

        # And the corrected forms, which must not be reported.
        open(planted, "w").write(
            "See [the plan](security-plan.md), [up](../CHANGELOG.md), "
            "[spec](specs/a.md), [web](https://example.com/x.md), "
            "[anchor](#part), [with anchor](security-plan.md#part).\n")
        clean = scan(d, ["docs/ROADMAP.md"])
        if clean:
            print(f"::error::correct links reported as broken: {clean}")
            fails += 1

        # Reference-style definitions are read too.
        open(planted, "w").write("Text [x].\n\n[x]: docs/security-plan.md\n")
        if len(scan(d, ["docs/ROADMAP.md"])) != 1:
            print("::error::a reference-style definition was not read")
            fails += 1

    print(f"selftest: {fails} failure(s)")
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    return check(subprocess.run(["git", "rev-parse", "--show-toplevel"],
                                capture_output=True, text=True,
                                check=True).stdout.strip())


if __name__ == "__main__":
    sys.exit(main())
