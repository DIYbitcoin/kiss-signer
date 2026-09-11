#!/usr/bin/env python3
"""Read every relative link in the tracked Markdown and HTML, and refuse one
that points at a file, or an anchor, that is not there.

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

WHAT IT CHECKS. In Markdown: every inline `[text](target)` and every
reference definition `[label]: target`. In HTML: every `href` and `src`. A
target is resolved against the directory of the file that holds it. http,
https, mailto and tel are left alone. Vendored upstream trees are skipped:
their links are their project's business and several are broken in the
copies we carry.

AND ANCHORS, which is the half this gate was blind to for its whole life.
It read `git ls-files "*.md"` and nothing else, so the five HTML pages that
ARE the published site went unchecked -- 63 links and 49 ids between them.
A bare `#anchor` was skipped outright and a `page.html#anchor` was checked
as far as `page.html`. The guide is thirteen topics reached only by their
anchors, so a renamed id is a link that silently goes nowhere.

An id is read out of the markup, so it does not matter that the guide keeps
twelve of its thirteen topics hidden at any moment: hidden is a property of
the rendered page and the id is in the file either way. Ids that JavaScript
assigns at runtime are invisible here by the same token, and that is
correct rather than a gap -- nothing written in the file can link to one,
because it does not exist until the page runs.

It reports how many files it read as well as how many links it rejected,
because a scan that finds nothing and a scan that reads nothing print the
same word.
"""

import argparse
import io
import os
import re
import subprocess
import sys
import tempfile

INLINE = re.compile(r"\[[^\]]*\]\(\s*<?([^)>\s]+)")
REFDEF = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*<?(\S+)>?\s*$", re.M)
SKIP_SCHEME = ("http://", "https://", "mailto:", "tel:", "ftp://", "data:",
               "javascript:")
# href and src both: a missing stylesheet or image is as broken as a missing
# page, and both are fetched by the reader's browser from the same directory.
HTML_ATTR = re.compile(r"""\b(?:href|src)\s*=\s*["']([^"']*)["']""")
HTML_ID = re.compile(r"""\bid\s*=\s*["']([^"']+)["']""")
# Vendored trees. Their broken links are upstream's, not ours.
VENDORED = ("components/libwally-core/upstream/", "managed_components/",
            "docs/installer/vendor/")


def targets(text):
    """Every link target in one file, inline and reference style."""
    return [m.group(1) for m in INLINE.finditer(text)] + REFDEF.findall(text)


def known_paths(root):
    """Every path git TRACKS, plus every directory one of them sits in.

    The filesystem is the wrong question, and asking it is the whole of the
    bug this function exists for. `managed_components/` is fetched by the
    component manager and gitignored, so it is on every developer's disk and
    in no clone and no CI checkout. A link into it resolved here, failed
    there, and sat red for six weeks because nobody could reproduce it.

    What a reader gets is what git carries. Asking that question gives the
    same answer on every machine, and it is also the honest one: a link to a
    path the repository does not hold is broken for whoever follows it,
    whatever happens to be lying around locally.
    """
    out = subprocess.run(["git", "-C", root, "ls-files", "-z"],
                         capture_output=True, text=True, check=True).stdout
    paths = set()
    for f in out.split("\0"):
        if not f:
            continue
        paths.add(f)
        d = os.path.dirname(f)
        while d:
            paths.add(d)
            d = os.path.dirname(d)
    return paths


def scan(root, files, known=None):
    """Return a list of (file, target) that do not resolve.

    `known` is the tracked path set; without one the check falls back to the
    filesystem, which is what the selftest's temporary tree wants.
    """
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
            resolved = os.path.normpath(os.path.join(here, target))
            ok = (not resolved.startswith("..")          # out of the repo
                  and (resolved in known if known is not None
                       else os.path.exists(os.path.join(root, resolved))))
            if not ok:
                bad.append((rel, raw))
    return bad


def tracked_markdown(root):
    out = subprocess.run(
        ["git", "-C", root, "ls-files", "*.md", "*.markdown"],
        capture_output=True, text=True, check=True).stdout.split()
    return [f for f in out if not f.startswith(VENDORED)]


def tracked_html(root):
    out = subprocess.run(
        ["git", "-C", root, "ls-files", "*.html"],
        capture_output=True, text=True, check=True).stdout.split()
    return [f for f in out if not f.startswith(VENDORED)]


def ids_in(root, files):
    """{file: {id, ...}} for every HTML file, so a fragment can be resolved.

    Read from the markup rather than from a rendered page on purpose. The
    guide hides twelve of its thirteen topics at a time, and an id in a
    hidden section is still an id.
    """
    found = {}
    for rel in files:
        try:
            text = open(os.path.join(root, rel),
                        encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        found[rel] = set(HTML_ID.findall(text))
    return found


def scan_html(root, files, known=None):
    """Return a list of (file, target, why) for links that do not resolve.

    Three ways to be wrong, and the gate was blind to all three: a path that
    the repository does not carry, a `#anchor` naming an id its own file does
    not have, and a `page.html#anchor` whose page exists and whose anchor
    does not.
    """
    bad = []
    ids = ids_in(root, files)
    for rel in files:
        try:
            text = open(os.path.join(root, rel),
                        encoding="utf-8", errors="replace").read()
        except OSError as e:
            bad.append((rel, f"<unreadable: {e}>", "could not be read"))
            continue
        here = os.path.dirname(rel)
        for raw in HTML_ATTR.findall(text):
            target = raw.strip()
            # "#" alone is the "top of page" idiom and names nothing
            if not target or target == "#" or target.startswith(SKIP_SCHEME):
                continue
            path, _, frag = target.partition("#")
            if path:
                resolved = os.path.normpath(os.path.join(here, path))
                ok = (not resolved.startswith("..")
                      and (resolved in known if known is not None
                           else os.path.exists(os.path.join(root, resolved))))
                if not ok:
                    bad.append((rel, raw, "no such path in the repository"))
                    continue
            else:
                resolved = rel          # a bare fragment is same-page
            # Only a file this scan read can be asked about its ids. A
            # fragment into a page outside the set is left alone rather than
            # guessed at.
            if frag and resolved in ids and frag not in ids[resolved]:
                bad.append((rel, raw, f"no id {frag!r} in {resolved}"))
    return bad


def check(root):
    md = tracked_markdown(root)
    html = tracked_html(root)
    # The trap this repository has been caught by before: a gate that looked
    # at nothing reported clean for its whole life. No files is a failure,
    # and it is asked of each kind separately, because reading every
    # Markdown file and no HTML is exactly the state this gate shipped in.
    if not md:
        print("::error::no tracked Markdown found -- this gate read nothing")
        return 1
    if not html:
        print("::error::no tracked HTML found -- this gate read nothing")
        return 1

    known = known_paths(root)
    bad_md = scan(root, md, known)
    bad_html = scan_html(root, html, known)

    print(f"read {len(md)} tracked Markdown file(s), "
          f"{len(html)} tracked HTML file(s)")
    print(f"broken relative links: {len(bad_md) + len(bad_html)}")
    for f, t in bad_md:
        print(f"  {f} -> {t}")
    for f, t, why in bad_html:
        print(f"  {f} -> {t}   ({why})")

    if bad_md:
        print("::error::a tracked Markdown file links to a path this "
              "repository does not carry. A link resolves from the file "
              "holding it, not from the repository root -- and a path that is "
              "only on your disk (managed_components/, build output) is not "
              "one the reader gets.")
    if bad_html:
        print("::error::a tracked HTML page links to a path or an anchor that "
              "is not there. These five pages are the published site, so this "
              "is a dead link a reader clicks, and an anchor is how every "
              "topic in the guide is reached.")
    return 1 if (bad_md or bad_html) else 0


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

        # THE ONE THIS GATE WAS BLIND TO: a link to a file that is on the disk
        # and not in the repository. Both forms are planted in one tree, so a
        # scan that answered from the filesystem would call both of them fine
        # -- which is exactly what shipped, and what CI then failed on.
        subprocess.run(["git", "-C", d, "init", "-q"], check=True)
        subprocess.run(["git", "-C", d, "add", "docs/security-plan.md"],
                       check=True)
        os.makedirs(os.path.join(d, "vendored"), exist_ok=True)
        open(os.path.join(d, "vendored", "upstream.c"), "w").close()
        open(planted, "w").write(
            "[tracked](security-plan.md) and "
            "[on disk only](../vendored/upstream.c)\n")
        got = scan(d, ["docs/ROADMAP.md"], known_paths(d))
        if len(got) != 1 or "upstream.c" not in got[0][1]:
            print(f"::error::an untracked link target was not caught: {got}")
            fails += 1

    # ---- the HTML half, and the three ways it can be wrong ----
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, "docs", "sim"))
        io.open(os.path.join(d, "docs", "guide.html"), "w").write(
            '<section id="how"></section>'
            '<a href="#how">fine</a>'
            '<a href="#nope">dead anchor, same page</a>'
            '<a href="sim/index.html">fine</a>')
        io.open(os.path.join(d, "docs", "index.html"), "w").write(
            '<a href="guide.html#how">fine</a>'
            '<a href="guide.html#gone">dead anchor, other page</a>'
            '<a href="missing.html">dead path</a>'
            '<a href="https://example.com/x#y">left alone</a>'
            '<a href="#">top of page, names nothing</a>')
        io.open(os.path.join(d, "docs", "sim", "index.html"), "w").write(
            '<a href="../index.html">fine</a>'
            '<img src="../media/x.png">')
        os.makedirs(os.path.join(d, "docs", "media"))
        open(os.path.join(d, "docs", "media", "x.png"), "w").close()
        subprocess.run(["git", "-C", d, "init", "-q"], check=True)
        subprocess.run(["git", "-C", d, "add", "docs"], check=True)

        pages = ["docs/guide.html", "docs/index.html", "docs/sim/index.html"]
        got = scan_html(d, pages, known_paths(d))
        want = {"#nope", "guide.html#gone", "missing.html"}
        if {t for _, t, _ in got} != want:
            print(f"::error::planted {sorted(want)}, scan found {got}")
            fails += 1

        # An id in a section the page hides is still an id. The guide keeps
        # twelve of thirteen topics hidden, so getting this wrong would
        # report the whole sidebar as broken.
        io.open(os.path.join(d, "docs", "guide.html"), "w").write(
            '<section id="how" hidden></section><a href="#how">x</a>')
        if scan_html(d, ["docs/guide.html"], known_paths(d)):
            print("::error::an id inside a hidden section was not seen")
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
