#!/usr/bin/env python3
"""Every valid last word for a drawn BIP39 phrase, computed off the device.

The BLIND DRAW flow asks the owner to draw 11 or 23 words blind from a physical
copy of the BIP39 list, then picks the final word from the list this prints.
Run it on a computer the signer has never touched: if the screen offers a word
this does not, the firmware is lying. The device shows 128 candidates for a 12 word seed and 8
for a 24 word one, and so does this.

    python3 tools/lastword.py word1 word2 ... word11
    python3 tools/lastword.py --check          # canonical BIP39 vectors

Reads the wordlist out of the vendored libwally source, so it agrees with the
firmware by construction. Standard library only, no network, no install.
"""
import hashlib
import pathlib
import sys

WORDS = (pathlib.Path(__file__).resolve().parent.parent
         / "components/libwally-core/upstream/src/data/wordlists/english.txt"
         ).read_text().split()
assert len(WORDS) == 2048, "wordlist is not BIP39 English"

INDEX = {w: i for i, w in enumerate(WORDS)}


def valid(mnemonic):
    """True when this full phrase satisfies the BIP39 checksum."""
    try:
        bits = "".join(f"{INDEX[w]:011b}" for w in mnemonic)
    except KeyError:
        return False
    if len(bits) % 33:
        return False
    ent_len = len(bits) * 32 // 33
    ent = int(bits[:ent_len], 2).to_bytes(ent_len // 8, "big")
    want = hashlib.sha256(ent).digest()[0] >> (8 - (len(bits) - ent_len))
    return int(bits[ent_len:], 2) == want


def candidates(drawn):
    """Every wordlist word that completes `drawn` into a valid phrase."""
    if len(drawn) not in (11, 23):
        raise SystemExit(f"need 11 or 23 words, got {len(drawn)}")
    unknown = [w for w in drawn if w not in INDEX]
    if unknown:
        raise SystemExit("not BIP39 words: " + " ".join(unknown))
    return [w for w in WORDS if valid(drawn + [w])]


def selftest():
    for n, want_count, want_word in ((11, 128, "about"), (23, 8, "art")):
        got = candidates(["abandon"] * n)
        assert len(got) == want_count, (n, len(got))
        assert want_word in got, (n, want_word)
        print(f"{n} x abandon -> {len(got)} candidates, includes {want_word}")
    print("vectors OK")


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        raise SystemExit(__doc__)
    if args[0] == "--check":
        selftest()
    else:
        got = candidates([w.lower() for w in args])
        print(f"{len(got)} of 2048 words fit yours:\n")
        for i in range(0, len(got), 8):
            print("  " + "  ".join(f"{w:<9}" for w in got[i:i + 8]))
