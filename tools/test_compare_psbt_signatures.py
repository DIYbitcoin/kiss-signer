#!/usr/bin/env python3
"""Adversarial export-comparison tests; all keys and fixtures are public."""

import ast
import base64
from copy import deepcopy
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

from compare_psbt_signatures import MAX_FILE, Refused, compare, parse

ROOT = Path(__file__).resolve().parent.parent


def compact(n):
    if n < 253:
        return bytes([n])
    return b"\xfd" + n.to_bytes(2, "little")


def encode(p, reverse=False):
    def mapping(m):
        pairs = list(m.items())
        if reverse:
            pairs.reverse()
        return b"".join(compact(len(k)) + k + compact(len(v)) + v for k, v in pairs) + b"\0"
    return b"psbt\xff" + mapping(p[0]) + b"".join(mapping(m) for m in p[1] + p[2])


def fixtures():
    # Existing independently generated embit vectors, not new expected values
    # computed by the comparison implementation.
    tree = ast.parse((ROOT / "tools/sign_fixtures/gen_sign_vectors.py").read_text())
    values = next(ast.literal_eval(n.value) for n in tree.body
                  if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and
                  t.id == "FIXTURES" for t in n.targets))
    header = (ROOT / "sim/sign_vectors.h").read_text()
    for name, raw in values.items():
        original = parse(bytes.fromhex(raw))
        signed = deepcopy(original)
        pub = next(k[1:] for k in original[1][0] if k[0] == 6)
        sig = re.search(r'#define SV_ECDSA_' + name + r' "([0-9a-f]+)"', header)[1]
        signed[1][0][b"\x02" + pub] = bytes.fromhex(sig)
        yield original, signed


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        self.original, self.signed = list(fixtures())[-1]

    def test_independent_vectors_and_serialization(self):
        for original, signed in fixtures():
            candidate = parse(base64.b64encode(encode(signed, reverse=True)) + b"\n")
            self.assertEqual(compare(original, signed, candidate), 1)

    def test_changed_nonce_bytes_are_refused(self):
        bad = deepcopy(self.signed)
        key = next(k for k in bad[1][0] if k[0] == 2)
        sig = bytearray(bad[1][0][key])
        sig[10] ^= 1
        bad[1][0][key] = bytes(sig)
        with self.assertRaisesRegex(Refused, "signature bytes"):
            compare(self.original, self.signed, bad)

    def test_changed_pubkey_is_refused(self):
        bad = deepcopy(self.signed)
        key = next(k for k in bad[1][0] if k[0] == 2)
        bad[1][0][key[:-1] + bytes([key[-1] ^ 1])] = bad[1][0].pop(key)
        with self.assertRaises(Refused):
            compare(self.original, self.signed, bad)

    def test_changed_transaction_even_on_both_exports_is_refused(self):
        bad = deepcopy(self.signed)
        tx = bytearray(bad[0][b"\0"])
        tx[-1] ^= 1
        bad[0][b"\0"] = bytes(tx)
        with self.assertRaisesRegex(Refused, "transaction"):
            compare(self.original, bad, bad)

    def test_added_leak_field_in_every_scope_is_refused(self):
        for scope in (0, 1, 2):
            bad = deepcopy(self.signed)
            mapping = bad[0] if scope == 0 else bad[scope][0]
            mapping[b"\xfc\x01x\x00"] = b"unexpected payload"
            with self.assertRaises(Refused):
                compare(self.original, self.signed, bad)

    def test_missing_signature_and_presigned_original_refused(self):
        with self.assertRaises(Refused):
            compare(self.original, self.signed, self.original)
        with self.assertRaises(Refused):
            compare(self.signed, self.signed, self.signed)

    def test_changed_utxo_refused(self):
        bad = deepcopy(self.signed)
        bad[1][0][b"\x01"] = b"changed amount or script"
        with self.assertRaises(Refused):
            compare(self.original, self.signed, bad)

    def test_v2_taproot_and_flag_clearing(self):
        original = ({b"\xfb": (2).to_bytes(4, "little"), b"\x02": (2).to_bytes(4, "little"),
                     b"\x04": b"\x01", b"\x05": b"\x01", b"\x06": b"\x03"},
                    [{b"\x0e": b"x" * 32, b"\x0f": b"\0" * 4}],
                    [{b"\x03": (1000).to_bytes(8, "little"), b"\x04": b"\x51"}])
        # Synthetic transport fixtures; comparison does not validate signatures.
        for size in (64, 65):
            signed = deepcopy(original)
            signed[0][b"\x06"] = b"\0"
            signed[1][0][b"\x13"] = b"s" * 64 + (b"\x01" if size == 65 else b"")
            self.assertEqual(compare(parse(encode(original)), parse(encode(signed)),
                                     parse(encode(signed))), 1)
            bad = deepcopy(signed)
            bad[0][b"\x06"] = b"\x04"
            with self.assertRaises(Refused):
                compare(original, signed, bad)
            bad = deepcopy(signed)
            bad[1][0][b"\x13"] = b"t" + bad[1][0][b"\x13"][1:]
            with self.assertRaises(Refused):
                compare(original, signed, bad)

    def test_unsupported_paths_refused(self):
        for field in (b"\x07", b"\x08", b"\x14" + b"x" * 64):
            bad = deepcopy(self.signed)
            bad[1][0][field] = b"x"
            with self.assertRaises(Refused):
                parse(encode(bad))

    def test_duplicate_noncanonical_trailing_and_truncated_refused(self):
        raw = encode(self.signed)
        tx = self.signed[0][b"\0"]
        duplicate = b"psbt\xff\x01\0" + compact(len(tx)) + tx + raw[5:]
        for bad in (duplicate, raw[:5] + b"\xfd\x01\0" + raw[6:], raw + b"\0",
                    b"x" * (MAX_FILE + 1), b"not base64"):
            with self.assertRaises(Refused):
                parse(bad)
        for end in range(len(raw)):
            with self.assertRaises(Refused):
                parse(raw[:end])

    def test_cli_success_failure_and_no_writes(self):
        with tempfile.TemporaryDirectory() as folder:
            paths = [Path(folder) / name for name in ("original.psbt", "reference.psbt", "candidate.psbt")]
            for path, psbt in zip(paths, (self.original, self.signed, self.signed)):
                path.write_bytes(encode(psbt))
            before = [p.read_bytes() for p in paths]
            command = [sys.executable, str(ROOT / "tools/compare_psbt_signatures.py"), *map(str, paths)]
            run = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("MATCH:", run.stdout)
            self.assertEqual(before, [p.read_bytes() for p in paths])
            paths[2].write_bytes(encode(self.original))
            run = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(run.returncode, 1)
            self.assertNotIn("MATCH:", run.stdout)


if __name__ == "__main__":
    unittest.main()
