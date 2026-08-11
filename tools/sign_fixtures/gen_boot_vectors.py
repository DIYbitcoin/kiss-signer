#!/usr/bin/env python3
"""Print the boot-selftest golden signatures for main/boot_sign_vectors.h.

Offline tool, not run in CI (the header it feeds is checked in). It computes
the two signatures the DEVICE must reproduce at boot, with an INDEPENDENT
implementation: pure Python, no embit, no libwally, no libsecp256k1. Only
hashlib. Nothing here traces back to KISS's own signer.

  - ECDSA follows the frozen rule in docs/specs/verifiable-determinism.md:
    RFC6979 (HMAC-SHA256 DRBG) with libsecp256k1's ndata convention, the
    counter ground in 32-byte little-endian extra entropy until R is low, and
    S normalised low. That is what EC_FLAG_GRIND_R does and what Core does.
  - Schnorr is the BIP340 reference signer from the spec, same code as the
    sibling gen_sp_sign_vectors.py, over a fixed aux.

The key and messages are derived from fixed ASCII labels so they are visibly
test values with no wallet behind them. They are NOT secret and NOT a seed.

Usage:
  python3 tools/sign_fixtures/gen_boot_vectors.py
"""

import hashlib
import hmac

P = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
G = (0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798,
     0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8)


def _inv(x, m):
    return pow(x, m - 2, m)


def _add(a, b):
    if a is None:
        return b
    if b is None:
        return a
    if a[0] == b[0] and a[1] != b[1]:
        return None
    if a == b:
        lam = (3 * a[0] * a[0] * _inv(2 * a[1], P)) % P
    else:
        lam = ((b[1] - a[1]) * _inv(b[0] - a[0], P)) % P
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


# --- RFC6979 HMAC-SHA256 DRBG, libsecp256k1's nonce_function_rfc6979 ---
# keydata = key32 || msg32 [|| ndata32]; algo16 is NULL on the ECDSA path.
class _Drbg:
    def __init__(self, keydata):
        self.v = b"\x01" * 32
        self.k = b"\x00" * 32
        self.k = hmac.new(self.k, self.v + b"\x00" + keydata, hashlib.sha256).digest()
        self.v = hmac.new(self.k, self.v, hashlib.sha256).digest()
        self.k = hmac.new(self.k, self.v + b"\x01" + keydata, hashlib.sha256).digest()
        self.v = hmac.new(self.k, self.v, hashlib.sha256).digest()
        self.retry = False

    def generate(self, n):
        if self.retry:
            self.k = hmac.new(self.k, self.v + b"\x00", hashlib.sha256).digest()
            self.v = hmac.new(self.k, self.v, hashlib.sha256).digest()
        out = b""
        while len(out) < n:
            self.v = hmac.new(self.k, self.v, hashlib.sha256).digest()
            out += self.v
        self.retry = True
        return out[:n]


def _nonce_rfc6979(msg32, key32, ndata):
    keydata = key32 + msg32 + (ndata if ndata is not None else b"")
    return _Drbg(keydata).generate(32)


def ecdsa_sign_grind(seckey, msg32):
    """(compact 64-byte r||s, grind counter). RFC6979 nonce ground for low R,
    S normalised low."""
    d = int.from_bytes(seckey, "big")
    z = int.from_bytes(msg32, "big")
    counter = 0
    ndata = None
    while True:
        k = int.from_bytes(_nonce_rfc6979(msg32, seckey, ndata), "big")
        if 0 < k < N:
            pt = _mul(G, k)
            r = pt[0] % N
            if r != 0:
                s = (_inv(k, N) * (z + r * d)) % N
                if s != 0:
                    if s > N // 2:          # libsecp256k1 always normalises S
                        s = N - s
                    if r < (1 << 255):      # first byte < 0x80: R is low
                        return _xb(r) + _xb(s), counter
        counter += 1
        ndata = counter.to_bytes(4, "little") + b"\x00" * 28


# --- BIP340 reference signer (from the spec; independent of libsecp256k1) ---
def _tagged(tag, m):
    t = hashlib.sha256(tag.encode()).digest()
    return hashlib.sha256(t + t + m).digest()


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


# Fixed test material. Visibly derived from labels, no wallet behind it.
# The message label is v2, not v1, on purpose: with v1 the RFC6979 counter-0
# nonce already yields a low R, so grinding never runs and the vector could not
# tell EC_FLAG_GRIND_R from plain RFC6979. v2's counter-0 R is high (0xcf...),
# so reproducing this vector requires the grind loop. main() asserts that.
KEY = hashlib.sha256(b"KISS boot selftest key v1").digest()
MSG = hashlib.sha256(b"KISS boot selftest message v2").digest()
AUX = hashlib.sha256(b"KISS boot selftest aux v1").digest()


def _carr(name, b):
    rows = []
    for i in range(0, len(b), 12):
        rows.append("    " + " ".join(f"0x{x:02x}," for x in b[i:i + 12]))
    return f"static const uint8_t {name}[{len(b)}] = {{\n" + "\n".join(rows) + "\n};"


def main():
    assert 0 < int.from_bytes(KEY, "big") < N
    ecdsa, counter = ecdsa_sign_grind(KEY, MSG)
    # A vector reachable at counter 0 would be satisfied by plain RFC6979 and
    # would not pin the low-R grinding half of the frozen rule.
    assert counter > 0, "message needs grinding to be a grind-R vector"
    print(_carr("BSV_KEY", KEY))
    print(_carr("BSV_MSG", MSG))
    print(_carr("BSV_AUX", AUX))
    print(f"// grind counter {counter}: plain RFC6979 alone cannot reach this R.")
    print(_carr("BSV_ECDSA", ecdsa))
    print(_carr("BSV_SCHNORR", schnorr_sign(MSG, KEY, AUX)))


if __name__ == "__main__":
    main()
