#!/usr/bin/env python3
"""Hold the baked art against the scripts that baked it.

    check_art_provenance.py              # check the tree
    check_art_provenance.py --update     # record today, after a real regen
    check_art_provenance.py --selftest

WHY NOT REGENERATE AND DIFF. Every other generator in this repo is gated by
running it again and comparing, and for these six that cannot work. They need
numpy and a macOS-only font, and font rasterisation is not byte reproducible
across machines anyway, so a regenerate-and-diff gate would go red on a Linux
runner for reasons that have nothing to do with drift. A gate that fails for
the wrong reason is one people learn to ignore, which is worse than no gate.

WHAT THIS DOES INSTEAD. It records a hash of each generator's sources and a
hash of each file that generator writes into the tree, and checks both are
still what they were. That catches the two ways this rots:

  * the generator moved and the art did not. Somebody edits menu_mock.py,
    never runs it, and main/menu_img.c goes on shipping the old picture with
    a script beside it that no longer describes it.
  * the art moved and the generator did not. A generated file edited by hand
    is a change the next regeneration silently throws away.

WHAT IT CANNOT DO, said out loud. It does not prove the committed art came
out of the committed generator; only a run on a machine with the fonts can do
that. It proves neither side has moved since a person last said they matched.
That is the whole claim, and it is the claim that was missing.

SHARED SOURCES COUNT. scene.py draws the backdrop for both menu_mock.py and
gameover_mock.py, so it is part of both their source hashes: a change there
moves two pictures and neither generator's own file would show it.

A GENERATOR NOT LISTED HERE IS A FINDING. That is the class fix rather than
the instance: six scripts were writing into main/ with nothing watching them
because nothing said they had to be watched. Adding a new one now fails this
check until somebody records what it writes, or records that it writes
nothing tracked.
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GENDIR = ROOT / "assets" / "generators"
MANIFEST = GENDIR / "provenance.json"

# What each generator writes into the tree. Declared, not discovered: these
# scripts cannot be imported without their dependencies, and a gate that
# needs numpy to tell you numpy is missing helps nobody. A generator whose
# list is empty writes only to /tmp or is a shared module.
OUTPUTS = {
    "convert_fruit.py": [],
    "flag_imgs.py":     ["main/flag_imgs.c", "main/flag_imgs.h"],
    "game_bg.py":       ["main/game_bg.c", "main/game_bg.h"],
    "gameover_mock.py": ["main/gameover_img.c", "main/gameover_img.h"],
    "kiss_mock.py":     ["main/kiss_img.c", "main/kiss_img.h",
                         "main/tile_lbls.c", "main/tile_lbls.h"],
    "menu_mock.py":     ["main/menu_img.c", "main/menu_img.h",
                         "main/menu_logo.c", "main/menu_logo.h"],
    "og_card.py":       ["docs/media/og-preview.png"],
    "scene.py":         [],
}


def sha(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def siblings(gen: Path, gendir: Path) -> list:
    """Modules this generator imports from its own directory, sorted.

    One level deep on purpose: the only shared module here is scene.py, and
    a transitive walk would be machinery with nothing to walk.
    """
    try:
        tree = ast.parse(gen.read_text(encoding="utf-8"))
    except (SyntaxError, UnicodeDecodeError):
        return []
    names = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            names.update(a.name.split(".")[0] for a in node.names)
        elif (isinstance(node, ast.ImportFrom) and node.module
              and not node.level):
            names.add(node.module.split(".")[0])
    return sorted(n for n in names
                  if (gendir / (n + ".py")).is_file()
                  and n + ".py" != gen.name)


def source_hash(gen: Path, gendir: Path) -> str:
    """One hash over the generator and every sibling it imports."""
    h = hashlib.sha256()
    for name in [gen.name] + [n + ".py" for n in siblings(gen, gendir)]:
        h.update(name.encode())
        h.update(sha(gendir / name).encode())
    return h.hexdigest()


def survey(root: Path, outputs: dict) -> dict:
    gendir = root / "assets" / "generators"
    out = {}
    for gen in sorted(gendir.glob("*.py")):
        entry = {"sources": [gen.name] + [n + ".py"
                                          for n in siblings(gen, gendir)],
                 "source_sha256": source_hash(gen, gendir),
                 "writes": {}}
        for rel in outputs.get(gen.name, []):
            p = root / rel
            entry["writes"][rel] = sha(p) if p.is_file() else "MISSING"
        out[gen.name] = entry
    return out


def check(root: Path, outputs: dict, manifest_path: Path):
    """(findings, checked) against the recorded manifest."""
    if not manifest_path.is_file():
        return ["no manifest at %s: run --update once to record today"
                % manifest_path.name], 0
    recorded = json.loads(manifest_path.read_text(encoding="utf-8"))
    now = survey(root, outputs)
    findings, checked = [], 0
    # A module another generator imports is already reported through that
    # generator, naming the picture that actually went stale. Reporting it
    # twice buries the useful half under the vague one.
    shared = {s for e in now.values() for s in e["sources"][1:]}

    for name in sorted(set(now) | set(recorded)):
        if name not in recorded:
            findings.append("%s is not in %s: a generator nothing watches. "
                            "Record what it writes, or record that it writes "
                            "nothing tracked." % (name, manifest_path.name))
            continue
        if name not in now:
            findings.append("%s is recorded but is not there any more: drop "
                            "it from %s." % (name, manifest_path.name))
            continue
        was, is_ = recorded[name], now[name]
        checked += 1
        src_moved = was.get("source_sha256") != is_["source_sha256"]
        art_moved = [rel for rel, digest in was.get("writes", {}).items()
                     if is_["writes"].get(rel) != digest]
        if src_moved and not art_moved and is_["writes"]:
            findings.append(
                "%s changed and the art it writes did not: %s still holds the "
                "picture the old script made. Regenerate, or say why not."
                % (name, ", ".join(sorted(was["writes"]))))
        elif src_moved and not is_["writes"] and name not in shared:
            findings.append(
                "%s changed. It writes nothing tracked, so nothing here can "
                "tell whether that mattered; re-record it deliberately."
                % name)
        for rel in art_moved:
            findings.append(
                "%s changed under %s: either it was edited by hand, which the "
                "next regeneration throws away, or it was regenerated without "
                "recording it." % (rel, name))
    return findings, checked


def selftest() -> int:
    import shutil
    import tempfile
    bad = 0

    def case(label, want, got):
        nonlocal bad
        ok = got == want
        print("  %-54s %s" % (label, "ok" if ok else "FAILED"))
        bad += not ok

    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        gendir = root / "assets" / "generators"
        gendir.mkdir(parents=True)
        (root / "main").mkdir()
        (gendir / "lib.py").write_text("W = 1\n")
        (gendir / "art.py").write_text("import lib\nprint(lib.W)\n")
        (root / "main" / "art.c").write_text("const int a = 1;\n")
        outs = {"art.py": ["main/art.c"], "lib.py": []}
        man = gendir / "provenance.json"

        man.write_text(json.dumps(survey(root, outs), indent=2, sort_keys=True))
        case("a tree that matches its manifest is clean",
             [], check(root, outs, man)[0])

        # The failure this exists for.
        (gendir / "art.py").write_text("import lib\nprint(lib.W + 1)\n")
        f = check(root, outs, man)[0]
        case("a generator that moved without its art is caught", 1, len(f))
        case("...and it names the file still holding the old picture", True,
             bool(f) and "main/art.c" in f[0])

        # A shared module counts as a source of everything that imports it.
        (gendir / "art.py").write_text("import lib\nprint(lib.W)\n")
        case("...and the same tree is clean again", [],
             check(root, outs, man)[0])
        (gendir / "lib.py").write_text("W = 2\n")
        case("a change in a shared module is caught too", 1,
             len(check(root, outs, man)[0]))
        (gendir / "lib.py").write_text("W = 1\n")

        # Art edited by hand.
        (root / "main" / "art.c").write_text("const int a = 2;\n")
        f = check(root, outs, man)[0]
        case("art edited by hand is caught", 1, len(f))
        case("...and it says the next regeneration would lose it", True,
             bool(f) and "throws away" in f[0])
        (root / "main" / "art.c").write_text("const int a = 1;\n")

        # The class fix: a new generator nobody recorded.
        (gendir / "new.py").write_text("print(1)\n")
        outs2 = dict(outs, **{"new.py": []})
        f = check(root, outs2, man)[0]
        case("a generator nothing watches is a finding", 1, len(f))
        case("...naming it", True, bool(f) and "new.py" in f[0])
        (gendir / "new.py").unlink()

        # A recorded generator that was deleted.
        shutil.move(str(gendir / "art.py"), str(gendir / "gone.py"))
        f = check(root, {"gone.py": [], "lib.py": []}, man)[0]
        case("a generator that is gone is a finding", True,
             any("not there any more" in x for x in f))

    print("art provenance selftest: %d broken" % bad)
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--update", action="store_true",
                    help="record the tree as it is now")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if args.update:
        MANIFEST.write_text(
            json.dumps(survey(ROOT, OUTPUTS), indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        print("recorded %d generator(s) -> %s"
              % (len(OUTPUTS), MANIFEST.relative_to(ROOT)))
        return 0
    findings, checked = check(ROOT, OUTPUTS, MANIFEST)
    for f in findings:
        print("FAIL: " + f)
    print("art provenance: %d generator(s) checked, %d finding(s)"
          % (checked, len(findings)))
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
