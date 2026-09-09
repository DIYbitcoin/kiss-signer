#!/usr/bin/env python3
"""Read the signature blocks an image actually carries, and hold them against
what the firmware was built to expect.

    check_sig_scheme.py <image.bin> --scheme rsa|ecdsa --blocks N
    check_sig_scheme.py <image.bin> --scheme rsa --blocks 3 --distinct
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
import hashlib
import zlib

SECTOR = 4096
BLOCK = 1216
MAGIC = 0xE7
VERSION = {"rsa": 0x02, "ecdsa": 0x03}
NAME = {v: k for k, v in VERSION.items()}

# Where the public key sits inside an RSA block: the modulus at 36, little
# endian, 384 bytes for RSA-3072, then the exponent in the next 4. Confirmed
# against a real three key bootloader rather than read off a diagram -- each
# block's modulus, reversed, is byte for byte the modulus openssl prints for
# the key that signed it, and the exponent read 65537 in all three.
#
# Only RSA. The ECDSA block puts a curve id and a shorter key somewhere else,
# and this project's release lane is RSA only, so --distinct refuses an ECDSA
# sector rather than digesting the wrong bytes and calling them a key.
RSA_KEY_SPAN = (36, 424)


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


def key_digests(image):
    """SHA-256 over each RSA block's public key, in block order.

    Raises ValueError on a sector this cannot read a key out of, because a
    digest of the wrong bytes would compare as happily as a right one.
    """
    sector = image[-SECTOR:]
    out = []
    for k in range(len(blocks(image))):
        blk = sector[k * BLOCK:(k + 1) * BLOCK]
        if blk[1] != VERSION["rsa"]:
            raise ValueError("block %d is %s; --distinct reads RSA blocks only"
                             % (k, NAME.get(blk[1], "unknown")))
        lo, hi = RSA_KEY_SPAN
        out.append(hashlib.sha256(blk[lo:hi]).hexdigest())
    return out


def distinct(image):
    """(ok, message): every signature block carries a different key.

    Three blocks are the whole of the rotation policy: secure boot burns a
    digest per block on first boot and revokes every slot it did not fill, so
    three copies of one key is a board with one key and no way back. The
    scheme and count checks both pass on that board, and so does espsecure's
    own verify, because each block really is a valid signature.
    """
    try:
        digests = key_digests(image)
    except ValueError as e:
        return False, str(e)
    if len(digests) < 2:
        return True, "%d signature block, nothing to compare" % len(digests)
    seen = {}
    for k, d in enumerate(digests):
        if d in seen:
            return False, ("blocks %d and %d carry the SAME public key: three "
                           "blocks are not three keys, and rotation needs "
                           "three" % (seen[d], k))
        seen[d] = k
    return True, "%d different keys" % len(seen)


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


def make_image(*schemes, trailer=b"", corrupt=False, keys=None):
    """keys: one filler byte per block, so two blocks can differ in their key
    span the way two real keys do. Left alone, every block is identical, which
    is itself the case --distinct exists to catch."""
    base = bytes(range(256)) * 40             # 10240 bytes, not aligned
    base += b"\xff" * (SECTOR - len(base) % SECTOR)
    fillers = keys or [b"\x5a"] * len(schemes)
    sector = b"".join(make_block(s, f) for s, f in zip(schemes, fillers))
    if corrupt and sector:
        sector = sector[:100] + bytes([sector[100] ^ 1]) + sector[101:]
    sector += b"\xff" * (SECTOR - len(sector))
    return base + (sector if schemes else b"") + trailer


def selftest():
    bad = 0
    ran = 0

    def case(label, want, ok):
        nonlocal bad, ran
        ran += 1
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

    # Three blocks are not three keys. Everything above passes on a bootloader
    # signed three times with one key file, which is a board that burns one
    # digest, revokes the other two slots, and can never be rotated.
    three = make_image("rsa", "rsa", "rsa",
                       keys=[b"\x11", b"\x22", b"\x33"])
    case("three different keys are distinct", True, distinct(three)[0])
    case("...and it says how many", True,
         "3 different keys" in distinct(three)[1])
    same = make_image("rsa", "rsa", "rsa", keys=[b"\x11", b"\x22", b"\x11"])
    ok, msg = distinct(same)
    case("the same key twice is refused", False, ok)
    case("...and names both blocks", True, "blocks 0 and 2" in msg)
    case("all three the same is refused", False,
         distinct(make_image("rsa", "rsa", "rsa"))[0])
    case("one block alone is not a duplicate", True,
         distinct(make_image("rsa"))[0])
    case("an ECDSA sector is refused rather than guessed", False,
         distinct(make_image("ecdsa", "ecdsa"))[0])
    case("...saying it reads RSA only", True,
         "RSA blocks only" in distinct(make_image("ecdsa", "ecdsa"))[1])

    print("signature scheme selftest: %d cases, %d broken" % (ran, bad))
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
        # Count first, then identity. A sector with the wrong number of blocks
        # has a more useful thing to say than "they are all different".
        if ok and "--distinct" in argv:
            ok, extra = distinct(image)
            msg = msg + ", " + extra
    print(("PASS: " if ok else "FAIL: ") + path.rsplit("/", 1)[-1] + ": " + msg)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
