#!/usr/bin/env python3
"""Is the published simulator built from the strings in this tree?

docs/sim/kiss-sim.wasm is the first link a visitor clicks and the README says
its screenshots come from the real firmware sources. That claim can only be
kept by a machine, and it was not: the committed bundle predated the def-row
work by months, drawing SETTINGS as four sans pill rows on a page that had
built five ruled rows with lamps for a long time.

The obvious check is to rebuild and compare hashes. It does not work here.
tools/build_wasm.sh is byte reproducible for ONE compiler -- that is what its
-g0 is for -- and emsdk versions differ between a laptop and a CI runner, so a
hash gate is red forever on the day either one moves.

So this asks the only question that actually matters: does the committed
binary contain the English strings this tree ships? A wasm built before a
string existed cannot contain it, and the strings are in the binary verbatim
(i18n_tables.c is a plain array of pointers to literals). It needs no
toolchain at all, which is why it can run in the same lane as the unit tests.

It does NOT prove the bundle is byte-current -- a code-only change leaves
every string in place. It proves the bundle is not from a different era, which
is the failure that happened.
"""
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
WASM = ROOT / "docs" / "sim" / "kiss-sim.wasm"
EN = ROOT / "i18n" / "en.json"

# EVERY English string long enough to be unmistakable, not a sample. Sampling
# the LONGEST was tried first and it proved nothing: this pass has spent its
# whole length replacing paragraphs with sentences, so the longest strings are
# the OLDEST ones and a bundle from before it passed cleanly. 24 characters is
# short enough to catch a rewritten fact value and long enough that a match is
# never a coincidence. Format strings are skipped -- the literal in the binary
# carries the % and the JSON does too, but a locale that reorders arguments
# does not, and this file is not the drift gate.
MIN_LEN = 24


# The comparison, as a function of its two inputs and nothing else, so the self
# test below can hand it a bundle it built rather than the one on disk.
def compare(blob: bytes, en: dict):
    want = {k: v for k, v in en.items()
            if isinstance(v, str) and len(v) >= MIN_LEN and "%" not in v}
    return want, {k: v for k, v in want.items() if v.encode("utf-8") not in blob}


# A gate is defined by what it EXCUSES, so the self test pins the exemptions as
# hard as it pins the failure. Two of these four must stay QUIET: MIN_LEN and
# the format-string skip are the whole reason this check does not fire on every
# bundle, and a version of it that reported them would be one nobody could act
# on. The other two are the failure it was written for -- a bundle from a
# different era, which is how the committed wasm drew SETTINGS as four sans
# pill rows months after the tree had built five ruled ones.
def selftest() -> int:
    long_a = "a fact value long enough to be unmistakable"
    long_b = "a second string of equally unmistakable length"
    cases = [
        ("a bundle carrying every long string, quiet",
         {"A": long_a, "B": long_b}, [long_a, long_b], 0),
        ("a bundle missing one, fires",
         {"A": long_a, "B": long_b}, [long_a], 1),
        ("a SHORT string absent, quiet -- under MIN_LEN",
         {"A": long_a, "S": "too short"}, [long_a], 0),
        ("a FORMAT string absent, quiet -- reordered by locale",
         {"A": long_a, "F": "%d " + long_b}, [long_a], 0),
    ]
    bad = 0
    for name, en, present, want_n in cases:
        blob = ("\x00".join(present)).encode("utf-8")
        _, missing = compare(blob, en)
        ok = len(missing) == want_n
        print(f"  {name:52s} {'ok' if ok else 'FAILED'} "
              f"({len(missing)} missing, wanted {want_n})")
        bad += not ok
    print(f"sim freshness selftest: {len(cases)} cases, {bad} broken")
    return 1 if bad else 0


def main() -> int:
    if "--selftest" in sys.argv:
        return selftest()
    if not WASM.exists():
        print(f"no bundle at {WASM.relative_to(ROOT)}")
        return 1
    blob = WASM.read_bytes()
    en = json.loads(EN.read_text(encoding="utf-8"))
    want, missing = compare(blob, en)
    if missing:
        print(f"simulator freshness: {len(missing)} of {len(want)} strings "
              f"are NOT in the committed bundle")
        for k, v in list(missing.items())[:5]:
            print(f'  missing  {k}  "{v[:60]}"')
        print("")
        print("docs/sim/kiss-sim.wasm predates this tree. Run:")
        print("    bash tools/build_wasm.sh")
        print("and commit docs/sim/kiss-sim.js and docs/sim/kiss-sim.wasm.")
        return 1
    print(f"simulator freshness: all {len(want)} strings present "
          f"({len(blob) // 1024} KB bundle)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
