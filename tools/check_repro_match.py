#!/usr/bin/env python3
"""Did two hosts build the same firmware, allowing only the bytes that cannot match?

The release build IS reproducible; its published SHA256 is not, and the two
facts get confused every time somebody checks. ESP-IDF stamps
`esp_app_desc_t.app_elf_sha256` into the image -- a hash of the ELF, debug
sections and all -- and the ELF's debug content depends on the host that made
it. Change one byte there and the image's own trailing SHA256, plus the single
ESP image checksum byte it covers, both change with it. So an arm64 Mac and an
amd64 runner building the same commit produce images whose every instruction
and every data byte is identical and whose sha256sum differs.

Measured that way on 2026-08-29 and never checked again, because nothing could
check it: CI built on one architecture only. A verifier on Apple silicon got a
mismatched hash, no job to compare against, and no way to tell a harmless
window from a real one.

This says which. It compares two images byte for byte and fails on ANY
difference outside the three windows above -- so the arm64 lane proves the
build reproduces rather than asserting it.

    python3 tools/check_repro_match.py A.bin B.bin
    python3 tools/check_repro_match.py --build-dirs DIR_A DIR_B
    python3 tools/check_repro_match.py --selftest

--build-dirs is the per-board form, one call per board's release build. It
compares every part flasher_args.json says makes up that board's flash -- the
app through the windows above, the bootloader, the partition table and the
ota data byte for byte -- and it refuses two directories that built different
boards before it compares a byte. With two boards there are two app names, and
an image checked against the other board's image differs everywhere, which
reads as a broken toolchain rather than as the wrong pair of files.

Exit 0 when the images differ only where they must.
"""
import json
import os
import sys

# esp_app_desc_t follows the 24-byte image header and the 8-byte header of the
# first segment; app_elf_sha256 is at offset 144 inside it. Layout from
# esp_app_format.h, same arithmetic as tools/check_fw_version.py.
DESC_OFF = 0x20
ELF_SHA_OFF = DESC_OFF + 144        # 0xB0
ELF_SHA_LEN = 32

# CONFIG_SECURE_SIGNED_APPS appends nothing here: an UNSIGNED image ends with
# the 32-byte image SHA256, and the byte before it is the ESP image checksum.
# Both are computed over everything above, so both move when the ELF hash does.
IMAGE_SHA_LEN = 32
CHECKSUM_LEN = 1


def windows(size):
    """(start, end) half-open byte ranges that are allowed to differ."""
    return [
        (ELF_SHA_OFF, ELF_SHA_OFF + ELF_SHA_LEN),
        (size - IMAGE_SHA_LEN - CHECKSUM_LEN, size - IMAGE_SHA_LEN),
        (size - IMAGE_SHA_LEN, size),
    ]


def name_of(off, size):
    for (lo, hi), label in zip(windows(size),
                               ("app_elf_sha256", "image checksum",
                                "image sha256")):
        if lo <= off < hi:
            return label
    return None


def compare(a, b):
    """(allowed, offending) byte counts, or a string when they cannot compare."""
    if len(a) != len(b):
        return f"images differ in LENGTH: {len(a)} vs {len(b)} bytes"
    if len(a) < DESC_OFF + 256:
        return f"image too short to hold an app descriptor: {len(a)} bytes"
    allowed = 0
    offending = []
    for off in range(len(a)):
        if a[off] == b[off]:
            continue
        if name_of(off, len(a)):
            allowed += 1
        else:
            offending.append(off)
    return allowed, offending


def load_parts(build_dir):
    """(app file, {offset: file}) from a build's flasher_args.json, or a string
    saying why that directory cannot be compared."""
    path = os.path.join(build_dir, "flasher_args.json")
    try:
        with open(path, encoding="utf-8") as f:
            args = json.load(f)
    except (OSError, ValueError) as exc:
        return f"{path}: {exc}"
    app = (args.get("app") or {}).get("file")
    parts = args.get("flash_files") or {}
    if not app or app not in parts.values():
        return f"{path} names no app among its flash files"
    return app, parts


def compare_dirs(dir_a, dir_b):
    """(lines, failed) for two release build directories of one board."""
    got_a, got_b = load_parts(dir_a), load_parts(dir_b)
    for got in (got_a, got_b):
        if isinstance(got, str):
            return [f"FAIL: {got}"], True
    app_a, parts_a = got_a
    app_b, parts_b = got_b
    if app_a != app_b:
        return [f"FAIL: {dir_a} built {app_a} and {dir_b} built {app_b}.",
                "      Those are two different boards, not two builds of one."], True
    if parts_a != parts_b:
        return ["FAIL: the two builds lay out flash differently:",
                f"      {dir_a}: {parts_a}",
                f"      {dir_b}: {parts_b}"], True

    lines, failed = [], False
    for off, name in sorted(parts_a.items(), key=lambda kv: int(kv[0], 16)):
        try:
            with open(os.path.join(dir_a, name), "rb") as f:
                a = f.read()
            with open(os.path.join(dir_b, name), "rb") as f:
                b = f.read()
        except OSError as exc:
            lines.append(f"FAIL: {name}: {exc}")
            failed = True
            continue
        if name != app_a:
            # No ELF hash in these, so nothing is allowed to move.
            if a == b:
                lines.append(f"PASS: {name} at {off} is byte-identical")
            else:
                lines.append(f"FAIL: {name} at {off} differs")
                failed = True
            continue
        got = compare(a, b)
        if isinstance(got, str):
            lines.append(f"FAIL: {name}: {got}")
            failed = True
        elif got[1]:
            lines.append(f"FAIL: {name} at {off}: {len(got[1])} byte(s) differ "
                         f"outside the allowed windows, first at {got[1][0]:#08x}")
            failed = True
        elif not got[0]:
            lines.append(f"PASS: {name} at {off} is byte-identical, "
                         f"{len(a)} bytes")
        else:
            lines.append(f"PASS: {name} at {off}, {len(a)} bytes, {got[0]} "
                         f"differ and all inside the allowed windows")
    if failed:
        lines.append("      These two hosts did not build the same firmware.")
    return lines, failed


def selftest():
    bad = 0
    cases = 0

    def check(name, got, want):
        nonlocal bad, cases
        ok = got == want
        print("  %-52s %s (%r)" % (name, "ok" if ok else "FAILED", got))
        bad += not ok
        cases += 1

    size = DESC_OFF + 256 + 4096
    base = (bytes(range(256)) * (size // 256 + 1))[:size]

    def poke(blob, off, n=1):
        out = bytearray(blob)
        for i in range(n):
            out[off + i] ^= 0xFF
        return bytes(out)

    ident = compare(base, base)
    check("identical images have nothing to report", ident, (0, []))

    # Each allowed window on its own, because the two at the tail are computed
    # from the first and a check that only knew about one would still pass the
    # pair that actually ships.
    got = compare(base, poke(base, ELF_SHA_OFF, ELF_SHA_LEN))
    check("the ELF hash window is allowed", got, (ELF_SHA_LEN, []))
    got = compare(base, poke(base, size - IMAGE_SHA_LEN, IMAGE_SHA_LEN))
    check("the trailing image sha256 is allowed", got, (IMAGE_SHA_LEN, []))
    got = compare(base, poke(base, size - IMAGE_SHA_LEN - 1))
    check("the image checksum byte is allowed", got, (1, []))

    # The whole point: one byte of code moving must not hide behind them.
    code = DESC_OFF + 256 + 10
    got = compare(base, poke(base, code))
    check("a byte of code is NOT allowed", got, (0, [code]))

    # The byte immediately below the ELF hash, which an off-by-one window eats.
    got = compare(base, poke(base, ELF_SHA_OFF - 1))
    check("the byte below the window is NOT allowed",
          got, (0, [ELF_SHA_OFF - 1]))
    got = compare(base, poke(base, ELF_SHA_OFF + ELF_SHA_LEN))
    check("the byte above the window is NOT allowed",
          got, (0, [ELF_SHA_OFF + ELF_SHA_LEN]))

    got = compare(base, base + b"\x00")
    check("a length difference is reported, not compared",
          isinstance(got, str), True)

    # The per-board form. Built on disk, because what it gets wrong would be
    # the reading of flasher_args.json and the choice of which part gets the
    # windows, and neither shows up in a comparison of two byte strings.
    import shutil
    import tempfile

    def build_dir(app, app_bytes, boot=b"boot" * 64):
        d = tempfile.mkdtemp()
        os.makedirs(os.path.join(d, "bootloader"))
        with open(os.path.join(d, "bootloader", "bootloader.bin"), "wb") as f:
            f.write(boot)
        with open(os.path.join(d, app), "wb") as f:
            f.write(app_bytes)
        with open(os.path.join(d, "flasher_args.json"), "w") as f:
            json.dump({"app": {"offset": "0x20000", "file": app},
                       "flash_files": {"0x2000": "bootloader/bootloader.bin",
                                       "0x20000": app}}, f)
        return d

    made = []
    try:
        a = build_dir("guition_kiss_bringup.bin", base)
        b = build_dir("guition_kiss_bringup.bin",
                      poke(base, ELF_SHA_OFF, ELF_SHA_LEN))
        made += [a, b]
        check("two builds of one board differing in the window",
              compare_dirs(a, b)[1], False)

        c = build_dir("guition_kiss_bringup.bin", base, boot=b"boot" * 63 + b"bopt")
        made.append(c)
        check("a bootloader byte is NOT allowed", compare_dirs(a, c)[1], True)

        # The same bytes under the other board's name: a matching image does
        # not excuse a pair of directories that built two different boards.
        w = build_dir("ws35_kiss_bringup.bin", base)
        made.append(w)
        check("builds of two different boards are refused",
              compare_dirs(a, w)[1], True)

        e = tempfile.mkdtemp()
        made.append(e)
        check("a directory with no flasher_args.json is refused",
              compare_dirs(a, e)[1], True)
    finally:
        for d in made:
            shutil.rmtree(d, ignore_errors=True)

    print("repro match selftest: %d cases, %d broken" % (cases, bad))
    return 1 if bad else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    if len(argv) == 4 and argv[1] == "--build-dirs":
        lines, failed = compare_dirs(argv[2], argv[3])
        for line in lines:
            print(line)
        if failed:
            return 1
        print(f"PASS: {argv[2]} and {argv[3]} built the same firmware.")
        return 0
    if len(argv) != 3:
        print(__doc__.strip())
        return 2
    with open(argv[1], "rb") as f:
        a = f.read()
    with open(argv[2], "rb") as f:
        b = f.read()

    got = compare(a, b)
    if isinstance(got, str):
        print(f"FAIL: {got}")
        return 1
    allowed, offending = got

    if offending:
        print(f"FAIL: {len(offending)} byte(s) differ OUTSIDE the windows that "
              f"are allowed to.")
        for off in offending[:16]:
            print(f"      {off:#08x}: {a[off]:#04x} vs {b[off]:#04x}")
        if len(offending) > 16:
            print(f"      ... and {len(offending) - 16} more")
        print("      These two hosts did not build the same firmware.")
        return 1

    print(f"PASS: {len(a)} bytes compared, {allowed} differ and all of them "
          f"are inside app_elf_sha256 and the two checksums it feeds.")
    print("      Every instruction and every data byte is identical.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
