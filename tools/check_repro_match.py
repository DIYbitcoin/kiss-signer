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
    python3 tools/check_repro_match.py --selftest

Exit 0 when the images differ only where they must.
"""
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


def selftest():
    bad = 0

    def check(name, got, want):
        nonlocal bad
        ok = got == want
        print("  %-52s %s (%r)" % (name, "ok" if ok else "FAILED", got))
        bad += not ok

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

    print("repro match selftest: 8 cases, %d broken" % bad)
    return 1 if bad else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
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
