#!/usr/bin/env python3
"""Draft the CHANGELOG entry for this release, and refuse a stale one.

    python3 tools/make_changelog.py --draft    # print an entry for VERSION
    python3 tools/make_changelog.py --check    # fail if the entry is missing

WHY THIS EXISTS. The entry was written by hand every release, which is the
shape every stale document in this repo has had: right only for as long as
somebody remembered. It was reported from the bench as beta9 reading like
beta8 -- and it did not, but nothing could have told anyone either way.

WHAT IT DOES NOT DO, on purpose. It does not write the sentence at the top.
Commit subjects say what changed; only a person can say what it means to
somebody holding the device, and a generated paragraph of those subjects
reads like a diff with adjectives. So --draft emits the bullets, the table
and a placeholder headline, and --check refuses the placeholder. The
automation is the skeleton and the refusal; the one line of meaning is
still written.

HOW IT GROUPS. By the files a commit touched, not by a prefix nobody types:

    security   crypto, the seed, firmware verification, duress
    screens    the UI sources and the theme kit
    words      i18n/en.json -- what the device says
    builders   tools/, sim/, .github/ -- nothing an owner sees
    docs       docs/ and the specs

A commit touching several lands in the first that matches, in that order,
so a screen change carrying a string change is a screen change.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHANGELOG = ROOT / "CHANGELOG.md"
VERSION = (ROOT / "VERSION").read_text(encoding="utf-8").strip()

PLACEHOLDER = "ONE LINE: what this release means to somebody holding the device."

# (key, emoji, heading, path test). Order is the precedence above.
GROUPS = [
    # The _ui exclusion is not tidiness. kiss_fw_ui.c and kiss_duress_ui.c
    # carry the substrings below and are SCREENS: the file that draws the
    # firmware page is not the file that verifies a signature. The selftest
    # caught this on the first run, which is the whole argument for having
    # one on a classifier nobody would otherwise check.
    ("security", "🔒", "Security",
     lambda p: "_ui." not in p and
               any(s in p for s in ("kiss_crypto", "kiss_seed", "kiss_fw",
                                    "kiss_duress", "kiss_kef", "pq_",
                                    "libwally", "secp256k1"))),
    ("screens", "🎨", "Screens",
     lambda p: p.startswith("main/") and p.endswith((".c", ".h"))),
    ("words", "📝", "Words on screen",
     lambda p: p == "i18n/en.json"),
    ("builders", "🧰", "For builders",
     lambda p: p.startswith(("tools/", "sim/", ".github/"))),
    ("docs", "📚", "Docs",
     lambda p: p.startswith(("docs/", "dev/")) or p.endswith(".md")),
]


def sh(*args: str) -> str:
    return subprocess.run(args, cwd=ROOT, capture_output=True,
                          text=True, check=True).stdout.strip()


def last_tag() -> str | None:
    """The newest tag that is an ancestor, excluding one for this VERSION.

    Excluded because a release tags itself: run this after tagging and the
    range would be empty, which reads as "nothing changed" rather than as
    the mistake it is.

    By VERSION order, and deliberately NOT --merged HEAD. Releases are tagged
    on main and drafted on develop, which does not contain main's merge
    commits -- so --merged skipped the last two releases and silently drafted
    against beta7, a thousand commits back. A range that wrong reads as a
    busy release rather than as a broken tool.
    """
    try:
        tags = sh("git", "tag", "--sort=-v:refname").split()
    except subprocess.CalledProcessError:
        return None
    for t in tags:
        if t.lstrip("v") != VERSION:
            return t
    return None


def commits(since: str | None) -> list[tuple[str, list[str]]]:
    """(subject, files) for each commit since `since`, newest last."""
    rng = f"{since}..HEAD" if since else "HEAD"
    raw = sh("git", "log", "--no-merges", "--reverse", "--name-only",
             "--format=%x00%s", rng)
    out = []
    for chunk in raw.split("\x00"):
        chunk = chunk.strip("\n")
        if not chunk:
            continue
        lines = chunk.split("\n")
        out.append((lines[0].strip(), [l for l in lines[1:] if l.strip()]))
    return out


def classify(files: list[str]) -> str:
    for key, _, _, test in GROUPS:
        if any(test(f) for f in files):
            return key
    return "builders"


def draft() -> str:
    since = last_tag()
    seen = commits(since)
    if not seen:
        raise SystemExit(f"no commits since {since}; nothing to draft")

    buckets: dict[str, list[str]] = {k: [] for k, _, _, _ in GROUPS}
    for subject, files in seen:
        buckets[classify(files)].append(subject)

    date = sh("git", "log", "-1", "--format=%cs")
    out = [f"## [{VERSION}], {date}", "", PLACEHOLDER, ""]

    # The table is the summary an owner reads: one row per area that moved,
    # with how many changes landed in it. Areas that did not move are absent
    # rather than listed as zero -- a row saying nothing happened is a row
    # somebody has to read before learning that.
    rows = [(e, h, len(buckets[k])) for k, e, h, _ in GROUPS if buckets[k]]
    if rows:
        out += ["| | |", "| --- | --- |"]
        out += [f"| {e} **{h}** | {n} change{'s' if n != 1 else ''} |"
                for e, h, n in rows]
        out.append("")

    for key, emoji, head, _ in GROUPS:
        if not buckets[key]:
            continue
        out.append(f"### {emoji} {head}")
        out.append("")
        # The subject line, as written. House rule already makes it a plain
        # imperative sentence under fifty characters, which is exactly the
        # shape a changelog bullet wants -- so it is copied, never reworded
        # by a script that cannot tell a fix from a rename.
        out += [f"- {s}" for s in buckets[key]]
        out.append("")

    out += [f"Since {since}." if since else "First release.", ""]
    return "\n".join(out)


def check() -> int:
    text = CHANGELOG.read_text(encoding="utf-8")
    m = re.search(rf"^## \[{re.escape(VERSION)}\][^\n]*\n(.*?)(?=^## \[|\Z)",
                  text, re.M | re.S)
    if not m:
        print(f"CHANGELOG.md has no entry for {VERSION}.")
        print("  draft one:  python3 tools/make_changelog.py --draft")
        return 1
    body = m.group(1).strip()
    if PLACEHOLDER in body:
        print(f"the {VERSION} entry still carries the placeholder headline.")
        print("  A generated list of commit subjects is not what a release")
        print("  means. Write the one line, then rerun.")
        return 1
    if len(body) < 80:
        print(f"the {VERSION} entry is {len(body)} characters, which is not an entry.")
        return 1
    print(f"changelog: {VERSION} has an entry, {len(body)} characters")
    return 0


def selftest() -> int:
    """Every rule fires on the string it was written for, and stays quiet on
    the one that fixed it. A gate that cannot fail is not a gate."""
    cases = [
        ("a screen source is a screen change",
         classify(["main/kiss_settings.c"]), "screens"),
        ("crypto wins over the screen it also touched",
         classify(["main/kiss_settings.c", "main/kiss_crypto.c"]), "security"),
        ("english strings alone are words",
         classify(["i18n/en.json"]), "words"),
        ("a screen carrying a string is a screen change",
         classify(["main/kiss_fw_ui.c", "i18n/en.json"]), "screens"),
        ("a workflow is for builders",
         classify([".github/workflows/ci.yml"]), "builders"),
        ("a spec is docs", classify(["docs/specs/kef-backup.md"]), "docs"),
        ("nothing recognised still lands somewhere",
         classify(["VERSION"]), "builders"),
    ]
    bad = 0
    for name, got, want in cases:
        ok = got == want
        bad += 0 if ok else 1
        print("  %-46s %s (%s)" % (name, "ok" if ok else "BROKEN", got))
    print("changelog selftest: %d cases, %d broken" % (len(cases), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--draft", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        sys.exit(selftest())
    if a.draft:
        print(draft())
        sys.exit(0)
    sys.exit(check())
