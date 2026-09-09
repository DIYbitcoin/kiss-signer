#!/usr/bin/env python3
"""Read every inline Python block in the release scripts and refuse one that
names something it never defines.

    check_release_lane.py            # check the release scripts in the tree
    check_release_lane.py --selftest

WHY THIS EXISTS. The release scripts carry their Python inline, as heredocs
fed to an interpreter. That code runs on release day and on no other day:
not in CI, not in preflight, not in any test. So a block can be committed
broken and stay broken until somebody is standing at the end of a Docker
build with a signing key in their hand.

That is exactly what happened. The block that rewrites the README version
badge went in with the beta9 publish, after beta9 had already been built. It
reaches for `os` and for `version`, and its heredoc imports neither. The next
release reached it eight minutes in, after the card had signed the firmware,
and died five frames deep in a traceback about a name.

WHAT IT CHECKS. Each block is parsed. Every name the block reads is held
against every name it binds -- imports, assignments, loop targets, `with ...
as`, comprehensions, function and class definitions, parameters -- plus the
builtins. A name that is read and never bound anywhere is the defect above,
and it is reported with the line the block starts on in the shell script.

Binding is collected without regard to order or branch, so a name assigned
under an `if` counts as bound. This checker is meant to catch the name that
is not there at all, and to never cry wolf about one that is.
"""

from __future__ import annotations

import argparse
import ast
import builtins
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# The scripts that carry inline Python, and the heredoc they open it with.
SCRIPTS = [
    "tools/make_web_release.sh",
    "tools/build_release.sh",
    "tools/build_encrypted_release.sh",
]
OPEN_RE = re.compile(r"<<'PY'\s*$")
CLOSE = "PY"

BUILTINS = set(dir(builtins)) | {"__file__", "__name__", "__doc__"}


def blocks(text: str):
    """(start_line, source) for each heredoc, start_line being the shell line
    the heredoc is opened on."""
    out, cur, start = [], None, 0
    for n, line in enumerate(text.splitlines(), 1):
        if cur is None:
            if OPEN_RE.search(line):
                cur, start = [], n
        elif line.strip() == CLOSE:
            out.append((start, "\n".join(cur) + "\n"))
            cur = None
        else:
            cur.append(line)
    return out


def bound_names(tree: ast.AST) -> set:
    """Every name the block binds, anywhere, in any scope."""
    names = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Name) and isinstance(node.ctx, (ast.Store,
                                                                ast.Del)):
            names.add(node.id)
        elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef,
                               ast.ClassDef)):
            names.add(node.name)
        elif isinstance(node, (ast.Import, ast.ImportFrom)):
            for a in node.names:
                names.add((a.asname or a.name).split(".")[0])
        elif isinstance(node, ast.arg):
            names.add(node.arg)
        elif isinstance(node, ast.ExceptHandler) and node.name:
            names.add(node.name)
        elif isinstance(node, ast.Global) or isinstance(node, ast.Nonlocal):
            names.update(node.names)
    return names


def undefined(source: str):
    """Names read and never bound. Raises SyntaxError on a block that will
    not parse, which is the other way one of these fails on release day."""
    tree = ast.parse(source)
    bound = bound_names(tree) | BUILTINS
    bad = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.Name) and isinstance(node.ctx, ast.Load):
            if node.id not in bound:
                bad.setdefault(node.id, node.lineno)
    return sorted(bad.items(), key=lambda kv: kv[1])


def check(paths) -> int:
    findings = 0
    checked = 0
    for rel in paths:
        p = ROOT / rel
        if not p.is_file():
            print(f"FAIL: {rel} is not there")
            findings += 1
            continue
        for start, src in blocks(p.read_text(encoding="utf-8")):
            checked += 1
            try:
                bad = undefined(src)
            except SyntaxError as e:
                print(f"FAIL: {rel}:{start}: the block does not parse: {e}")
                findings += 1
                continue
            for name, line in bad:
                print(f"FAIL: {rel}:{start}: the block reads {name!r} "
                      f"(its own line {line}) and never defines it")
                findings += 1
    print(f"release lane: {checked} inline blocks checked, {findings} bad")
    return 1 if findings else 0


def selftest() -> int:
    fails = 0

    def chk(what, cond):
        nonlocal fails
        if not cond:
            print(f"FAIL: {what}")
            fails += 1

    # The real defect: a name from a module that was never imported.
    chk("a missing import is caught",
        [n for n, _ in undefined("import re\nif os.path.exists('x'):\n"
                                 "    re.sub('a', 'b', 'c')\n")] == ["os"])
    # The same block's second defect: a bare name nothing assigns.
    chk("a name nothing assigns is caught",
        [n for n, _ in undefined("b = open('x').read()\n"
                                 "b.replace(found, version)\n")]
        == ["found", "version"])
    # Bound under a branch, read after it. Not a defect.
    chk("a name bound in a branch is not a finding",
        undefined("import os\nif os.sep:\n    v = 1\nelse:\n    v = 2\n"
                  "print(v)\n") == [])
    # Loop targets, with-as, comprehensions, parameters, exception names.
    chk("the other binding forms are understood",
        undefined("import json, pathlib\n"
                  "def f(a, b=1):\n    return a + b\n"
                  "for x in [1]:\n    print(x, f(x))\n"
                  "with open('p') as fh:\n    print([y for y in fh])\n"
                  "try:\n    json.loads('{}')\n"
                  "except ValueError as e:\n    print(e, pathlib)\n") == [])
    # A block that does not parse is a finding, not a crash.
    try:
        undefined("if True\n    pass\n")
        chk("an unparsable block raises", False)
    except SyntaxError:
        pass

    # The extractor: two blocks out of a shell file, and text outside them
    # is not read as Python.
    shell = ("echo hi\n"
             "\"$PY\" - <<'PY'\n"
             "import os\n"
             "PY\n"
             "echo between\n"
             "VERSION=\"$VERSION\" \"$PY\" - <<'PY'\n"
             "print(1)\n"
             "PY\n"
             "echo done\n")
    got = blocks(shell)
    chk("both heredocs are found", len(got) == 2)
    chk("the first is located at its opening line", got and got[0][0] == 2)
    chk("the second carries only its own body",
        len(got) == 2 and got[1][1].strip() == "print(1)")

    # End to end, through the file reader, on a script with a known defect.
    with tempfile.TemporaryDirectory() as d:
        bad = Path(d) / "bad.sh"
        bad.write_text("\"$PY\" - <<'PY'\nprint(nope)\nPY\n", encoding="utf-8")
        global ROOT
        keep, ROOT = ROOT, Path(d)
        rc = check(["bad.sh"])
        ROOT = keep
        chk("a defective script exits nonzero", rc == 1)

    # And the scripts in the tree, which is the check itself.
    chk("the release scripts are clean", check(SCRIPTS) == 0)

    print(f"selftest: {fails} failure(s)")
    return 1 if fails else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    return selftest() if args.selftest else check(SCRIPTS)


if __name__ == "__main__":
    sys.exit(main())
