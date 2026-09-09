#!/usr/bin/env python3
"""Read the signature blocks an image actually carries, and hold them against
what the firmware was built to expect.

    check_sig_scheme.py <image.bin> --scheme rsa|ecdsa --blocks N
    check_sig_scheme.py <image.bin> --unsigned
    check_sig_scheme.py --selftest

Every signature check in the release scripts used to verify an image against
the key that signed it, and every one of them passed on an image the board
would have refused: the config said RSA-3072, the key was secp256r1, and
espsecure printed PASS because the block really was a valid ECDSA block. Not
one check asked what the FIRMWARE expects. This is that check. It reads the
tail sector, validates each block the way the bootloader does (magic, version,
CRC32 over the first 1196 bytes) and compares scheme and count with the recipe.

The count matters as much as the scheme. Secure boot v2 burns the digest of
every key in the BOOTLOADER's signature sector on first boot and revokes every
slot it did not fill, so "three keys, rotation by revocation" is a promise the
bootloader either carries in bytes or does not carry at all. --blocks 3 turns
the spec's sentence into an assertion.

--unsigned is the same reader pointed the other way: a build directory that
still holds last run's signed image gets signed AGAIN, and the second sector
hides behind the first. Asked before signing, this refuses an image that is
already carrying blocks.

Layout constants are espsecure's (esptool 5.3.1): a 4096 byte sector on the
end of a sector aligned image, up to three 1216 byte blocks, magic 0xE7,
version byte 0x02 for RSA-PSS and 0x03 for ECDSA. Runs before the post
quantum trailer, which sits after the sector and breaks the alignment this
reader depends on; a trailer already present is reported, not skipped.
"""
import struct
import sys
import zlib

SECTOR = 4096
BLOCK = 1216
MAGIC = 0xE7
VERSION = {"rsa": 0x02, "ecdsa": 0x03}
NAME = {v: k for k, v in VERSION.items()}


def blocks(image):
    """Scheme names of the valid blocks in the tail sector, in order.

    Stops at the first invalid block, which is how the bootloader reads them.
    An image that is not sector aligned cannot carry a sector at all.
    """
    if len(image) < SECTOR or len(image) % SECTOR:
        return []
    sector = image[-SECTOR:]
    out = []
    for k in range(3):
        blk = sector[k * BLOCK:(k + 1) * BLOCK]
        magic, version = blk[0], blk[1]
        (crc,) = struct.unpack("<I", blk[1196:1200])
        if magic != MAGIC or version not in NAME:
            break
        if crc != zlib.crc32(blk[:1196]) & 0xFFFFFFFF:
            break
        out.append(NAME[version])
    return out


def judge(image, scheme=None, count=None, unsigned=False):
    """(ok, message). Pure, so the self test can drive it without files."""
    found = blocks(image)
    if unsigned:
        if found:
            return False, ("already carries %d signature block(s) (%s): stale "
                           "build output, delete the image and rebuild"
                           % (len(found), ", ".join(found)))
        return True, "no signature block, as built"
    if len(found) != count:
        hint = ""
        if not found and len(image) % SECTOR:
            hint = " (not sector aligned: a trailer after the sector?)"
        return False, ("%d signature block(s), recipe expects %d%s"
                       % (len(found), count, hint))
    wrong = [s for s in found if s != scheme]
    if wrong:
        return False, ("signature scheme %s, firmware expects %s: the board "
                       "would refuse this image on first boot"
                       % ("/".join(found), scheme))
    return True, "%d %s signature block(s)" % (count, scheme.upper())


def make_block(scheme, filler=b"\x5a"):
    body = bytes([MAGIC, VERSION[scheme], 0, 0]) + filler * (1196 - 4)
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF) + b"\x00" * 16


def make_image(*schemes, trailer=b"", corrupt=False):
    base = bytes(range(256)) * 40             # 10240 bytes, not aligned
    base += b"\xff" * (SECTOR - len(base) % SECTOR)
    sector = b"".join(make_block(s) for s in schemes)
    if corrupt and sector:
        sector = sector[:100] + bytes([sector[100] ^ 1]) + sector[101:]
    sector += b"\xff" * (SECTOR - len(sector))
    return base + (sector if schemes else b"") + trailer


def selftest():
    bad = 0

    def case(label, want, ok):
        nonlocal bad
        good = ok == want
        print("  %-52s %s" % (label, "ok" if good else "FAILED"))
        bad += not good

    # The failure this file exists to catch: a valid ECDSA block on a build
    # whose config says RSA. espsecure passes it; this must not.
    case("an ECDSA block on an RSA recipe is refused", False,
         judge(make_image("ecdsa"), "rsa", 1)[0])
    case("an ECDSA block on the ECDSA recipe passes", True,
         judge(make_image("ecdsa"), "ecdsa", 1)[0])
    case("three RSA blocks satisfy --blocks 3", True,
         judge(make_image("rsa", "rsa", "rsa"), "rsa", 3)[0])
    case("one RSA block does not satisfy --blocks 3", False,
         judge(make_image("rsa"), "rsa", 3)[0])
    case("a mixed sector is refused", False,
         judge(make_image("rsa", "ecdsa"), "rsa", 2)[0])
    case("a block with a bad CRC does not count", False,
         judge(make_image("rsa", corrupt=True), "rsa", 1)[0])
    case("a trailer after the sector is reported", False,
         judge(make_image("rsa", trailer=b"PQ" * 50), "rsa", 1)[0])
    case("an unsigned image is unsigned", True,
         judge(make_image(), unsigned=True)[0])
    case("a signed image is not unsigned", False,
         judge(make_image("rsa"), unsigned=True)[0])
    print("signature scheme selftest: 9 cases, %d broken" % bad)
    return 1 if bad else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    if len(argv) < 2:
        sys.exit(__doc__)
    path = argv[1]
    image = open(path, "rb").read()
    if "--unsigned" in argv:
        ok, msg = judge(image, unsigned=True)
    else:
        try:
            scheme = argv[argv.index("--scheme") + 1]
            count = int(argv[argv.index("--blocks") + 1])
        except (ValueError, IndexError):
            sys.exit(__doc__)
        if scheme not in VERSION or not 1 <= count <= 3:
            sys.exit(__doc__)
        ok, msg = judge(image, scheme, count)
    print(("PASS: " if ok else "FAIL: ") + path.rsplit("/", 1)[-1] + ": " + msg)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
