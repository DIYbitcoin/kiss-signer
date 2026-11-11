#!/usr/bin/env python3
"""Compare actual exports from two independent deterministic signers.

Usage: python3 tools/compare_psbt_signatures.py original.psbt reference.psbt candidate.psbt

Offline, stdlib only. Accepts binary or base64 PSBTv0/v2 files. Requires
unfinalized exports, every input signed, and unchanged non-signature fields
(except clearing v2 input/output modification flags). Unsupported shapes fail
closed. This is byte comparison, NOT signature validation or anti-exfil: the
reference must come from an independently trusted signer using the same keys
and deterministic signing rules. Never give this tool a seed or private key.
"""

import argparse
import base64
import binascii
from pathlib import Path


MAX_FILE = 2 * 1024 * 1024
MAX_ITEMS = 10000
SIG_TYPES = {0x02, 0x13}


class Refused(ValueError):
    pass


class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def take(self, size):
        if size < 0 or size > len(self.data) - self.pos:
            raise Refused("truncated data")
        result = self.data[self.pos:self.pos + size]
        self.pos += size
        return result

    def compact(self):
        first = self.take(1)[0]
        if first < 253:
            return first
        width, minimum = {253: (2, 253), 254: (4, 65536), 255: (8, 2**32)}[first]
        value = int.from_bytes(self.take(width), "little")
        if value < minimum:
            raise Refused("noncanonical compact size")
        return value

    def end(self):
        if self.pos != len(self.data):
            raise Refused("unexpected trailing data")

    def mapping(self):
        result = {}
        while True:
            size = self.compact()
            if not size:
                return result
            if len(result) >= MAX_ITEMS:
                raise Refused("too many map entries")
            key = self.take(size)
            kr = Reader(key)
            if kr.compact() > 252:
                raise Refused("unsupported extended key type")
            if key in result:
                raise Refused("duplicate PSBT key")
            result[key] = self.take(self.compact())


def number(mapping, key, width):
    value = mapping.get(key)
    if value is None or len(value) != width:
        raise Refused("missing or malformed required field")
    return int.from_bytes(value, "little")


def count(value):
    reader = Reader(value)
    n = reader.compact()
    reader.end()
    if not 0 < n <= MAX_ITEMS:
        raise Refused("unsupported input/output count")
    return n


def transaction_counts(raw):
    reader = Reader(raw)
    reader.take(4)
    ni = reader.compact()
    if not 0 < ni <= MAX_ITEMS:
        raise Refused("unsupported transaction input count or witness serialization")
    for _ in range(ni):
        reader.take(36)
        if reader.compact() != 0:
            raise Refused("original transaction contains scriptSig")
        reader.take(4)
    no = reader.compact()
    if not 0 < no <= MAX_ITEMS:
        raise Refused("unsupported transaction output count")
    for _ in range(no):
        reader.take(8)
        reader.take(reader.compact())
    reader.take(4)
    reader.end()
    return ni, no


def parse(raw):
    if len(raw) > MAX_FILE:
        raise Refused("PSBT exceeds size limit")
    if not raw.startswith(b"psbt\xff"):
        try:
            raw = base64.b64decode(b"".join(raw.split()), validate=True)
        except (ValueError, binascii.Error) as exc:
            raise Refused("expected binary or base64 PSBT") from exc
    reader = Reader(raw)
    if reader.take(5) != b"psbt\xff":
        raise Refused("not a PSBT")
    glob = reader.mapping()
    version = number(glob, b"\xfb", 4) if b"\xfb" in glob else 0
    if version == 0:
        if b"\x00" not in glob or any(k[0] in range(2, 7) for k in glob):
            raise Refused("invalid PSBTv0 global fields")
        ni, no = transaction_counts(glob[b"\x00"])
    elif version == 2:
        if b"\x00" in glob:
            raise Refused("PSBTv2 contains unsigned transaction")
        number(glob, b"\x02", 4)
        ni = count(glob.get(b"\x04", b""))
        no = count(glob.get(b"\x05", b""))
        if b"\x06" in glob and number(glob, b"\x06", 1) & ~7:
            raise Refused("unsupported transaction modification flags")
    else:
        raise Refused("unsupported PSBT version")
    inputs = [reader.mapping() for _ in range(ni)]
    outputs = [reader.mapping() for _ in range(no)]
    reader.end()
    for item in inputs:
        if any(key[0] in (7, 8, 0x14) for key in item):
            raise Refused("finalized or taproot script-path inputs are unsupported")
        if version == 2:
            number(item, b"\x0e", 32)
            number(item, b"\x0f", 4)
    if version == 2:
        for item in outputs:
            number(item, b"\x03", 8)
            if b"\x04" not in item:
                raise Refused("unresolved PSBTv2 output")
    return glob, inputs, outputs


def signatures(item):
    result = {}
    for key, value in item.items():
        if key[0] == 2:
            if not ((len(key) == 34 and key[1] in (2, 3)) or
                    (len(key) == 66 and key[1] == 4)) or not 9 <= len(value) <= 73:
                raise Refused("malformed ECDSA signature field")
            result[key] = value
        elif key[0] == 0x13:
            if len(key) != 1 or len(value) not in (64, 65):
                raise Refused("malformed taproot signature field")
            result[key] = value
    return result


def checked_signatures(original, signed):
    og, oi, oo = original
    sg, si, so = signed
    sg = dict(sg)
    # BIP370 signers clear the input/output modifiable bits. No other global
    # change is accepted, including a newly set bit or a changed sighash bit.
    if b"\x06" in og and b"\x06" in sg:
        before, after = og[b"\x06"][0], sg[b"\x06"][0]
        if after != before and after != (before & ~3):
            raise Refused("unexpected modification flags")
        sg[b"\x06"] = og[b"\x06"]
    if sg != og or so != oo or len(si) != len(oi):
        raise Refused("transaction or global/output metadata changed")
    result = []
    for i, (before, after) in enumerate(zip(oi, si)):
        if signatures(before):
            raise Refused("original must contain no signatures")
        sigs = signatures(after)
        if not sigs:
            raise Refused(f"input {i}: no signature; partial coverage is unsupported")
        rest = {k: v for k, v in after.items() if k[0] not in SIG_TYPES}
        if rest != before:
            raise Refused(f"input {i}: non-signature fields changed")
        result.append(sigs)
    return result


def compare(original, reference, candidate):
    expected = checked_signatures(original, reference)
    actual = checked_signatures(original, candidate)
    for i, (left, right) in enumerate(zip(expected, actual)):
        if left != right:
            raise Refused(f"input {i}: signature bytes or signing keys differ; do not broadcast")
    return sum(map(len, expected))


def load(path):
    with Path(path).open("rb") as stream:
        return parse(stream.read(MAX_FILE + 1))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("original", help="original unsigned PSBT")
    parser.add_argument("reference", help="export from independently trusted signer")
    parser.add_argument("candidate", help="export being checked")
    args = parser.parse_args()
    try:
        total = compare(load(args.original), load(args.reference), load(args.candidate))
    except (OSError, Refused) as exc:
        print(f"REFUSED: {exc}")
        return 1
    print(f"MATCH: {total} signature(s) match the reference byte for byte.")
    print("Only meaningful with an independently trusted reference and original PSBT.")
    print("Not signature validation, automatic broadcast protection, or proof against all leaks.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
