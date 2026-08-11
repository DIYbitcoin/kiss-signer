#!/usr/bin/env python3
"""Does components/cUR/CMakeLists.txt list a set that actually links?

sim/build_test.sh compiles `components/cUR/src/types/*.c` by glob and never
reads CMakeLists.txt, so a file dropped from the COMPONENT build is invisible
to every desktop gate: kisstest stays green while the ESP32 link fails. That is
not hypothetical -- an audit cleanup dropped src/types/bytes_type.c while
keeping src/types/psbt.c, whose psbt_new/psbt_free are thin wrappers over
bytes_new/bytes_free, and nothing here noticed.

This compiles exactly the files CMakeLists names, links them into one relocatable
object, and fails if anything in that set calls something the set does not
define. Host clang, not the cross toolchain: the question is which translation
units are present, and that answer is the same on both.

    python3 tools/check_cur_link.py
"""
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMP = ROOT / "components" / "cUR"

# Symbols the component legitimately gets from libc. Kept as an allowlist
# rather than a denylist: an unknown name should fail loudly and be added here
# deliberately, because the whole point is catching a missing translation unit.
EXTERNAL = re.compile(
    r"^_?("
    r"malloc|calloc|realloc|free|"
    r"mem(cpy|set|move|cmp|chr)|"
    r"str(len|cmp|ncmp|cpy|ncpy|cat|ncat|chr|rchr|str|dup|tol)|"
    r"snprintf|sprintf|printf|fprintf|puts|putchar|"
    r"is(digit|xdigit|alpha|alnum|lower|upper|space|print)|"
    r"to(lower|upper)|"
    r"abs|labs|strtoul|strtol|strtoull|qsort|abort|exit|assert.*|"
    r"__.*|___.*|_?dyld.*"
    r")$"
)


def main() -> int:
    cmake = (COMP / "CMakeLists.txt").read_text()
    srcs = re.findall(r'"(src/[^"]+\.c)"', cmake)
    if not srcs:
        print("check_cur_link: no SRCS found in CMakeLists.txt", file=sys.stderr)
        return 1

    missing = [s for s in srcs if not (COMP / s).exists()]
    if missing:
        print("check_cur_link: listed but not on disk: " + ", ".join(missing))
        return 1

    tmp = Path(tempfile.mkdtemp(prefix="curlink"))
    try:
        objs = []
        for s in srcs:
            obj = tmp / (Path(s).name + ".o")
            r = subprocess.run(
                ["clang", "-c", "-w", f"-I{COMP}/src", f"-I{ROOT}/sim/shims",
                 str(COMP / s), "-o", str(obj)],
                capture_output=True, text=True)
            if r.returncode:
                print(f"check_cur_link: {s} does not compile\n{r.stderr[:400]}")
                return 1
            objs.append(str(obj))

        merged = tmp / "all.o"
        r = subprocess.run(["clang", "-r", "-o", str(merged)] + objs,
                           capture_output=True, text=True)
        if r.returncode:
            print("check_cur_link: merge failed\n" + r.stderr[:400])
            return 1

        out = subprocess.run(["nm", "-u", str(merged)],
                             capture_output=True, text=True).stdout
        unresolved = sorted({
            ln.strip() for ln in out.splitlines()
            if ln.strip() and not EXTERNAL.match(ln.strip())
        })
        if unresolved:
            print("check_cur_link: the CMakeLists set calls what it does not "
                  "define, so the firmware link fails while kisstest stays green:")
            for u in unresolved:
                print("  " + u)
            print("\nA source was dropped from components/cUR/CMakeLists.txt "
                  "while something still listed there calls into it.")
            return 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"cUR component link: {len(srcs)} sources, no unresolved symbols")
    return 0


if __name__ == "__main__":
    sys.exit(main())
