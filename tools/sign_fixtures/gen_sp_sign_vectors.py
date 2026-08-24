#!/usr/bin/env python3
"""Print the golden silent-payment Schnorr signatures for sim/sign_vectors.h.

Offline tool, not run in CI. It reproduces KISS's silent-payment spend
signatures with an INDEPENDENT implementation, so the pinned bytes the C test
asserts are not KISS's own output fed back.

Independence has three legs, none of which touch KISS's signer:
  - the taproot sighash comes from main/sp_spend_vectors.h, which the sibling
    gen_spend_vectors.py computed with embit;
  - the spend key is derived from the dev mnemonic with embit (BIP352 path
    m/352'/1'/0'/0'/0, testnet, which is the network the SP spend test runs on),
    and the BIP376 tweak is read straight out of the PSBT;
  - the signature itself is produced by the BIP340 reference implementation
    below (from the BIP340 spec, pure Python, independent of libsecp256k1),
    with aux_rand all zero -- BIP340's standard deterministic nonce, which is
    the only shape sp_schnorr_sign can produce.

Because the aux is the standard one, this script is no longer the only thing
that can reproduce a KISS taproot signature: any conforming BIP340 signer with
the same key does. If KISS's nonce derivation or tweak math ever drift, these
bytes stop matching and the build fails.

Only the 64-byte SIGHASH_DEFAULT form is produced; the explicit-SIGHASH_ALL
variant would need an independently computed ALL sighash and is left to the
existing encoding tests in sim/test_sp.c.

Setup:
  python3 -m venv venv && ./venv/bin/pip install embit
Usage:
  ./venv/bin/python tools/sign_fixtures/gen_sp_sign_vectors.py
"""

import base64
import hashlib
import re
import sys
from pathlib import Path

try:
    from embit import bip39, bip32
except ImportError:
    sys.exit("need embit: python3 -m venv venv && ./venv/bin/pip install embit")

HDR = Path(__file__).resolve().parents[2] / "main" / "sp_spend_vectors.h"
MNEMONIC = ("abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon abandon abandon about")

# --- BIP340 reference signer (from the spec; independent of libsecp256k1) ---
P = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
G = (0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798,
     0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8)


def _tagged(tag, m):
    t = hashlib.sha256(tag.encode()).digest()
    return hashlib.sha256(t + t + m).digest()


def _inv(x):
    return pow(x, P - 2, P)


def _add(a, b):
    if a is None:
        return b
    if b is None:
        return a
    if a[0] == b[0] and a[1] != b[1]:
        return None
    if a == b:
        lam = (3 * a[0] * a[0] * _inv(2 * a[1])) % P
    else:
        lam = ((b[1] - a[1]) * _inv(b[0] - a[0])) % P
    x = (lam * lam - a[0] - b[0]) % P
    return (x, (lam * (a[0] - x) - a[1]) % P)


def _mul(pt, k):
    r = None
    while k:
        if k & 1:
            r = _add(r, pt)
        pt = _add(pt, pt)
        k >>= 1
    return r


def _xb(x):
    return x.to_bytes(32, "big")


def schnorr_sign(msg, seckey, aux):
    d0 = int.from_bytes(seckey, "big")
    pt = _mul(G, d0)
    d = d0 if pt[1] % 2 == 0 else N - d0
    t = d ^ int.from_bytes(_tagged("BIP0340/aux", aux), "big")
    rnd = _tagged("BIP0340/nonce", t.to_bytes(32, "big") + _xb(pt[0]) + msg)
    k0 = int.from_bytes(rnd, "big") % N
    r = _mul(G, k0)
    k = k0 if r[1] % 2 == 0 else N - k0
    e = int.from_bytes(_tagged("BIP0340/challenge", _xb(r[0]) + _xb(pt[0]) + msg), "big") % N
    return _xb(r[0]) + _xb((k + e * d) % N)


def main():
    src = HDR.read_text()

    def b64(name):
        return re.search(rf'{name}\[\] = "([^"]+)"', src).group(1)

    def arr(name):
        body = re.search(rf'{name}\[32\] = \{{([^}}]+)\}}', src).group(1)
        return bytes(int(x, 16) for x in re.findall(r"0x[0-9a-f]{2}", body))

    root = bip32.HDKey.from_seed(bip39.mnemonic_to_seed(MNEMONIC))
    spend_priv = root.derive("m/352h/1h/0h/0h/0").secret

    for tag, macro in (("EVEN", "SV_SCHNORR_SP_EVEN"), ("ODD", "SV_SCHNORR_SP_ODD ")):
        raw = base64.b64decode(b64(f"SPV_SPEND_{tag}_B64"))
        sighash = arr(f"SPV_SPEND_{tag}_SIGHASH")
        outkey = arr(f"SPV_SPEND_{tag}_OUTKEY")
        # BIP376 tweak: PSBT input field, key 0x20 (keylen 1), value 32 bytes.
        i = raw.find(b"\x01\x20\x20")
        tweak = raw[i + 3:i + 3 + 32]
        d = (int.from_bytes(spend_priv, "big") + int.from_bytes(tweak, "big")) % N
        assert _xb(_mul(G, d)[0]) == outkey, f"{tag}: d*G x != output key"
        sig = schnorr_sign(sighash, d.to_bytes(32, "big"), bytes(32)).hex()
        print(f'#define {macro} "{sig}"')


if __name__ == "__main__":
    main()
