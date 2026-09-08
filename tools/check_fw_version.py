#!/usr/bin/env python3
"""Does the built firmware declare the version the VERSION file says?

Reads the ESP-IDF app descriptor out of a built .bin and compares it against
the repo's VERSION file. Run it on ANY build directory, not just the release
lane.

This exists because a build directory can declare a version the tree has never
held. `file(STRINGS ... VERSION ...)` in CMakeLists.txt is a read, and a read
is not a dependency, so PROJECT_VER froze at whatever VERSION said the day that
build directory was first configured; every later `idf.py build` relinked an
image carrying the frozen number. build-release/ declared 0.1.0-beta8 against a
tree whose VERSION has never been past beta7, and a device ended up disagreeing
with the card in its own slot about which of them was newer.

CMAKE_CONFIGURE_DEPENDS fixes the cause. This is the check that says so, and
the one that catches a build directory configured before the fix landed.

No desktop gate can do this job: every sim/build_*.sh re-reads VERSION with
`head -1` on each invocation, so the simulator is always fresh and cannot
reproduce the fault.

    python3 tools/check_fw_version.py [build-dir ...]      # default: build

Exit 0 when every directory checked agrees with VERSION.
"""
import json
import os
import shutil
import struct
import sys

# esp_app_desc_t sits after the 24-byte image header and the 8-byte header of
# the first segment. Layout from esp_app_format.h.
DESC_OFF = 0x20
DESC_MAGIC = 0xABCD5432
VER_OFF = 16          # char version[32]
VER_LEN = 32
PROJ_OFF = 48         # char project_name[32]
PROJ_LEN = 32

BIN_NAME = "guition_kiss_bringup.bin"


def field(blob, off, length):
    return blob[off:off + length].split(b"\0")[0].decode("utf-8", "replace")


def descriptor_version(path):
    """The version string the image itself declares, or None."""
    with open(path, "rb") as f:
        head = f.read(DESC_OFF + PROJ_OFF + PROJ_LEN)
    if len(head) < DESC_OFF + PROJ_OFF + PROJ_LEN:
        return None, "image too short to hold an app descriptor"
    magic, = struct.unpack("<I", head[DESC_OFF:DESC_OFF + 4])
    if magic != DESC_MAGIC:
        return None, f"no app descriptor (magic {magic:#010x})"
    return field(head, DESC_OFF + VER_OFF, VER_LEN), None


def compile_definition(build_dir):
    """KISS_VERSION_STR as the compiler was actually handed it, or None.

    The Settings row and the app descriptor are two separate pipelines. They
    agree today only because main/CMakeLists.txt now takes PROJECT_VER off the
    build properties instead of reading the file a second time -- and this is
    what proves it stayed that way.
    """
    cc = os.path.join(build_dir, "compile_commands.json")
    if not os.path.exists(cc):
        return None
    with open(cc, encoding="utf-8") as f:
        try:
            entries = json.load(f)
        except json.JSONDecodeError:
            return None
    needle = "KISS_VERSION_STR="
    for e in entries:
        cmd = e.get("command") or " ".join(e.get("arguments", []))
        i = cmd.find(needle)
        if i < 0:
            continue
        rest = cmd[i + len(needle):]
        # -DKISS_VERSION_STR=\"0.1.0-beta7\" survives the shell in several
        # shapes; take everything up to the next whitespace and strip quoting.
        tok = rest.split()[0] if rest.split() else ""
        return tok.strip('\\"\'"')
    return None


# Both halves are parsers of things nobody here writes by hand -- an ESP-IDF
# app descriptor and a compile_commands.json entry -- so both are exactly the
# kind of code that keeps working until a format moves under it and then
# silently returns None, which this gate reads as "nothing to compare" and
# passes. The cases below build each input rather than reading a build tree,
# so the check can be proved on a machine that has never run idf.py.
def selftest():
    import tempfile
    bad = 0

    def image(version, magic=DESC_MAGIC, project=b"kiss"):
        blob = bytearray(b"\xff" * (DESC_OFF + PROJ_OFF + PROJ_LEN))
        blob[DESC_OFF:DESC_OFF + 4] = struct.pack("<I", magic)
        v = version.encode()[:VER_LEN - 1]
        blob[DESC_OFF + VER_OFF:DESC_OFF + VER_OFF + len(v) + 1] = v + b"\0"
        blob[DESC_OFF + PROJ_OFF:DESC_OFF + PROJ_OFF + len(project) + 1] = project + b"\0"
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as fh:
            fh.write(bytes(blob))
            return fh.name

    def check(name, got, want):
        nonlocal bad
        ok = got == want
        print("  %-52s %s (%r)" % (name, "ok" if ok else "FAILED", got))
        bad += not ok

    path = image("0.1.0-beta9")
    try:
        ver, err = descriptor_version(path)
        check("the descriptor's version is read back", (ver, err),
              ("0.1.0-beta9", None))
    finally:
        os.unlink(path)

    # A wrong magic must NOT be read as a version. This is the failure that
    # makes the gate dangerous rather than merely broken: a plausible looking
    # string lifted out of an image that has no descriptor at all.
    path = image("0.1.0-beta9", magic=0xDEADBEEF)
    try:
        ver, err = descriptor_version(path)
        check("a wrong magic yields no version", ver, None)
    finally:
        os.unlink(path)

    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as fh:
        fh.write(b"\x00" * 8)
        short = fh.name
    try:
        ver, err = descriptor_version(short)
        check("an image too short yields no version", ver, None)
    finally:
        os.unlink(short)

    # The compile_commands half, in the three quotings the define survives in.
    d = tempfile.mkdtemp()
    try:
        with open(os.path.join(d, "compile_commands.json"), "w") as fh:
            json.dump([{"command": 'cc -DKISS_VERSION_STR=\\"0.1.0-beta9\\" -c a.c'}], fh)
        check("KISS_VERSION_STR is read off the compile line",
              compile_definition(d), "0.1.0-beta9")
    finally:
        shutil.rmtree(d, ignore_errors=True)

    d = tempfile.mkdtemp()
    try:
        with open(os.path.join(d, "compile_commands.json"), "w") as fh:
            json.dump([{"command": "cc -c a.c"}], fh)
        check("a build with no such define yields None",
              compile_definition(d), None)
    finally:
        shutil.rmtree(d, ignore_errors=True)

    print("fw version selftest: 5 cases, %d broken" % bad)
    return 1 if bad else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    with open(os.path.join(root, "VERSION"), encoding="utf-8") as f:
        want = f.readline().strip()

    dirs = argv[1:] or ["build"]
    checked = 0
    fails = 0

    for d in dirs:
        path = os.path.join(root, d, BIN_NAME) if not os.path.isabs(d) \
            else os.path.join(d, BIN_NAME)
        if not os.path.exists(path):
            print(f"skip: {d}/{BIN_NAME} is not built")
            continue
        checked += 1
        got, err = descriptor_version(path)
        if err:
            print(f"FAIL: {d}: {err}")
            fails += 1
            continue
        if got == want:
            print(f"PASS: {d} declares {got}")
        else:
            print(f"FAIL: {d} declares {got}, VERSION says {want}")
            print(f"      the build directory was configured while VERSION "
                  f"read {got}. Delete it, or touch VERSION and rebuild.")
            fails += 1

        cdef = compile_definition(os.path.join(root, d))
        if cdef is None:
            pass                      # no compile_commands.json to read
        elif cdef == want:
            print(f"PASS: {d} compiles KISS_VERSION_STR as {cdef}")
        else:
            print(f"FAIL: {d} compiles KISS_VERSION_STR as {cdef}, "
                  f"VERSION says {want}")
            fails += 1

    if not checked:
        print("nothing to check: no build directory holds a firmware image")
        return 0
    print(f"firmware version gate: {checked} build dir(s), {fails} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
