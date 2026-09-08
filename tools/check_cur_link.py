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


def link_set(paths, incs):
    """Compile and merge `paths`, and return (unresolved, error).

    Split out of main so the self test can drive the REAL machinery -- clang
    -c, clang -r, nm and the allowlist -- over sources it wrote itself. A self
    test that only checked the regex would prove nothing about the half of this
    gate that has actually broken, which was nm's output format differing
    between macOS and the CI runner.
    """
    tmp = Path(tempfile.mkdtemp(prefix="curlink"))
    try:
        objs = []
        for src in paths:
            obj = tmp / (Path(src).name + ".o")
            cmd = ["clang", "-c", "-w"] + [f"-I{i}" for i in incs] + \
                  [str(src), "-o", str(obj)]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode:
                return None, f"{src} does not compile\n{r.stderr[:400]}"
            objs.append(str(obj))

        merged = tmp / "all.o"
        r = subprocess.run(["clang", "-r", "-o", str(merged)] + objs,
                           capture_output=True, text=True)
        if r.returncode:
            return None, "merge failed\n" + r.stderr[:400]

        out = subprocess.run(["nm", "-u", str(merged)],
                             capture_output=True, text=True).stdout
        # GNU nm prints "                 U symbol"; BSD/macOS nm prints the
        # bare name. Take the last field either way -- stripping alone leaves
        # the "U " on Linux and then nothing matches the allowlist, which is
        # how this gate failed CI the first time it ran there.
        names = {ln.split()[-1] for ln in out.splitlines() if ln.split()}
        return sorted(n for n in names if not EXTERNAL.match(n)), None
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# The failure this gate exists for, built from scratch: one source calling into
# another, and the set that forgot to list the callee. That is exactly the
# shape of the audit cleanup that dropped src/types/bytes_type.c while keeping
# psbt.c, whose psbt_new is a thin wrapper over bytes_new.
#
# The third case is the allowlist, which is what stops this gate reporting
# every libc call in the component. A version of it without that exemption
# would fire on everything; a version that had lost the allowlist's anchoring
# would fire on nothing.
def selftest() -> int:
    bad = 0
    tmp = Path(tempfile.mkdtemp(prefix="curlinkself"))
    try:
        (tmp / "callee.c").write_text("int cur_helper(int x) { return x + 1; }\n")
        (tmp / "caller.c").write_text(
            "int cur_helper(int x);\n"
            "int cur_entry(int x) { return cur_helper(x); }\n")
        (tmp / "libc_only.c").write_text(
            "#include <string.h>\n"
            "void cur_copy(char *a, const char *b) { memcpy(a, b, 4); }\n")

        cases = [
            ("a set that lists the callee, quiet",
             [tmp / "caller.c", tmp / "callee.c"], []),
            ("a set that dropped the callee, fires",
             [tmp / "caller.c"], ["cur_helper"]),
            ("a set that only calls libc, quiet -- the allowlist",
             [tmp / "libc_only.c"], []),
        ]
        for name, paths, want in cases:
            got, err = link_set(paths, [str(tmp)])
            if err:
                print("  %-52s FAILED (%s)" % (name, err.splitlines()[0]))
                bad += 1
                continue
            # nm decorates on macOS; compare on the undecorated names.
            got = [g.lstrip("_") for g in got]
            ok = got == want
            print("  %-52s %s (%s)" % (name, "ok" if ok else "FAILED", got))
            bad += not ok
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("cUR link selftest: 3 cases, %d broken" % bad)
    return 1 if bad else 0


def main() -> int:
    if "--selftest" in sys.argv:
        return selftest()
    cmake = (COMP / "CMakeLists.txt").read_text()
    srcs = re.findall(r'"(src/[^"]+\.c)"', cmake)
    if not srcs:
        print("check_cur_link: no SRCS found in CMakeLists.txt", file=sys.stderr)
        return 1

    missing = [s for s in srcs if not (COMP / s).exists()]
    if missing:
        print("check_cur_link: listed but not on disk: " + ", ".join(missing))
        return 1

    unresolved, err = link_set([COMP / s for s in srcs],
                               [f"{COMP}/src", f"{ROOT}/sim/shims"])
    if err:
        print("check_cur_link: " + err)
        return 1
    if unresolved:
        print("check_cur_link: the CMakeLists set calls what it does not "
              "define, so the firmware link fails while kisstest stays green:")
        for u in unresolved:
            print("  " + u)
        print("\nA source was dropped from components/cUR/CMakeLists.txt "
              "while something still listed there calls into it.")
        return 1

    print(f"cUR component link: {len(srcs)} sources, no unresolved symbols")
    return 0


if __name__ == "__main__":
    sys.exit(main())
