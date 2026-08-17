#!/usr/bin/env python3
"""Open and build KEF v20 (AES-256-GCM) envelopes off-device.

This is the interop half of the KEF backup tests: an envelope the firmware
makes that this tool cannot open — or one built here that the firmware
refuses — is a format break, and a device-only roundtrip can never see it.
The AES here is its own pure-Python implementation (S-box derived from the
GF(2^8) construction, not a pasted table), so the C code and this file can
only agree by both being right. With no arguments it self-tests against the
same golden vectors baked into sim/test_kef.c, which were produced by the
format's reference implementation.

    python3 tools/kef_roundtrip.py
    python3 tools/kef_roundtrip.py wrap <password> <id> <iterations> <plain_hex> [iv_hex]
    python3 tools/kef_roundtrip.py unwrap <password> <envelope_hex>

No dependencies. Iterations are the effective count (e.g. 100000); the
stored 3-byte form follows the format's rule automatically.
"""
import hashlib
import os
import sys

# ---- AES-256, encrypt direction only (GCM needs nothing else) -----------


def _make_sbox():
    sbox = [0] * 256
    p = q = 1
    while True:
        # p walks the multiplicative group by *3, q by /3 (its inverse)
        p = (p ^ (p << 1) ^ (0x1B if p & 0x80 else 0)) & 0xFF
        q ^= (q << 1) & 0xFF
        q ^= (q << 2) & 0xFF
        q ^= (q << 4) & 0xFF
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        s = q
        for r in (1, 2, 3, 4):
            s ^= ((q << r) | (q >> (8 - r))) & 0xFF
        sbox[p] = s ^ 0x63
        if p == 1:
            break
    sbox[0] = 0x63
    return bytes(sbox)


_SBOX = _make_sbox()


def _xtime(a):
    return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1


def _expand_key(key):
    words = [key[i:i + 4] for i in range(0, 32, 4)]
    rcon = 1
    for i in range(8, 60):
        w = words[i - 1]
        if i % 8 == 0:
            w = bytes(_SBOX[b] for b in w[1:] + w[:1])
            w = bytes([w[0] ^ rcon]) + w[1:]
            rcon = _xtime(rcon)
        elif i % 8 == 4:
            w = bytes(_SBOX[b] for b in w)
        words.append(bytes(a ^ b for a, b in zip(words[i - 8], w)))
    return [b"".join(words[i:i + 4]) for i in range(0, 60, 4)]


def _encrypt_block(round_keys, block):
    state = [b ^ k for b, k in zip(block, round_keys[0])]
    for rnd in range(1, 15):
        state = [_SBOX[b] for b in state]
        # rows live at stride 4 in the byte order AES specifies
        state = [state[(i + 4 * (i % 4)) % 16] for i in range(16)]
        if rnd != 14:
            mixed = []
            for c in range(0, 16, 4):
                a = state[c:c + 4]
                mixed += [
                    _xtime(a[0]) ^ _xtime(a[1]) ^ a[1] ^ a[2] ^ a[3],
                    a[0] ^ _xtime(a[1]) ^ _xtime(a[2]) ^ a[2] ^ a[3],
                    a[0] ^ a[1] ^ _xtime(a[2]) ^ _xtime(a[3]) ^ a[3],
                    _xtime(a[0]) ^ a[0] ^ a[1] ^ a[2] ^ _xtime(a[3]),
                ]
            state = mixed
        state = [b ^ k for b, k in zip(state, round_keys[rnd])]
    return bytes(state)


# ---- GCM ----------------------------------------------------------------


def _gmul(x, y):
    z, v = 0, y
    for i in range(127, -1, -1):
        if (x >> i) & 1:
            z ^= v
        v = (v >> 1) ^ (0xE1 << 120) if v & 1 else v >> 1
    return z


def _ghash(h, data):
    y = 0
    for i in range(0, len(data), 16):
        block = data[i:i + 16].ljust(16, b"\x00")
        y = _gmul(y ^ int.from_bytes(block, "big"), h)
    return y


def _gcm(key, iv, data):
    """CTR-xor `data`; returns (out, round_keys, H, J0)."""
    rk = _expand_key(key)
    h = int.from_bytes(_encrypt_block(rk, bytes(16)), "big")
    j0 = iv + b"\x00\x00\x00\x01"
    out = bytearray()
    ctr = int.from_bytes(j0[12:], "big")
    for i in range(0, len(data), 16):
        ctr = (ctr + 1) & 0xFFFFFFFF
        ks = _encrypt_block(rk, iv + ctr.to_bytes(4, "big"))
        out += bytes(a ^ b for a, b in zip(data[i:i + 16], ks))
    return bytes(out), rk, h, j0


def _gcm_tag(rk, h, j0, ct):
    s = _gmul(_ghash(h, ct) ^ (len(ct) * 8), h)
    ek = int.from_bytes(_encrypt_block(rk, j0), "big")
    return (s ^ ek).to_bytes(16, "big")


def gcm_seal(key, iv, plain):
    ct, rk, h, j0 = _gcm(key, iv, plain)
    return ct, _gcm_tag(rk, h, j0, ct)


def gcm_open(key, iv, ct, tag4):
    plain, rk, h, j0 = _gcm(key, iv, ct)
    if _gcm_tag(rk, h, j0, ct)[:4] != tag4:
        return None
    return plain


# ---- the envelope -------------------------------------------------------


def _stored_iterations(effective):
    if effective % 10000 == 0 and 1 <= effective // 10000 <= 10000:
        return effective // 10000
    if 10000 < effective < 2 ** 24:
        return effective
    raise SystemExit("iterations out of the storable range")


def wrap(password, id_, effective, plain, iv=None):
    if len(id_) > 252:
        raise SystemExit("id over 252 bytes")
    iv = iv or os.urandom(12)
    if len(iv) != 12:
        raise SystemExit("iv must be 12 bytes")
    key = hashlib.pbkdf2_hmac("sha256", password.encode(), id_, effective)
    ct, tag = gcm_seal(key, iv, plain)
    return (bytes([len(id_)]) + id_ + bytes([20])
            + _stored_iterations(effective).to_bytes(3, "big")
            + iv + ct + tag[:4])


def unwrap(password, env):
    if len(env) < 22:
        raise SystemExit("Failed!")
    lid = env[0]
    if len(env) < 1 + lid + 4 + 17:
        raise SystemExit("Failed!")
    id_, version = env[1:1 + lid], env[1 + lid]
    if version != 20:
        raise SystemExit("Failed!")     # decrypt vague: no version hints
    raw = int.from_bytes(env[2 + lid:5 + lid], "big")
    eff = raw * 10000 if raw <= 10000 else raw
    if eff < 10000:
        raise SystemExit("Failed!")
    if eff > 1000000:
        print("note: over the firmware's 1M iteration cap; opening anyway",
              file=sys.stderr)
    payload = env[5 + lid:]
    key = hashlib.pbkdf2_hmac("sha256", password.encode(), id_, eff)
    plain = gcm_open(key, payload[:12], payload[12:-4], payload[-4:])
    if plain is None:
        raise SystemExit("Failed!")
    return id_, eff, plain


# ---- self test ----------------------------------------------------------

GOLD = [
    ("test password", "000102030405060708090a0b",
     "00112233445566778899aabbccddeeff",
     "0837334335444130411400000a000102030405060708090a0b"
     "1ca567170a34839c507e98865210aba80de4fc5a", True),
    ("KISS", "f0e1d2c3b4a5968778695a4b",
     "7f0623ab29bd6f9a9895e9ac8b57d2ee0af21c1e7fa4b1c0eafe0c9d2b6f7d31",
     "0014002711f0e1d2c3b4a5968778695a4bcc419726eba0ef6230dbe8d3b95ad9"
     "a33486c2b831c31fb31be383d7c03e1414e85c5ae2", True),
    ("satóshi nakamóto", "000102030405060708090a0b",
     "637261776c2061696d2062726965662067617370206272696566206d696e6420"
     "6a617a7a2062656c7420746f6e677565206b6e6f77207374617920746f6e65",
     "066261636b757014000019000102030405060708090a0b01745064a89f26c45f"
     "a20099afcf0f2fe00699a29d520075d2aaa4ca9112a244975c2b54b7a16988e2"
     "90bd90c475cf11ceb392a7ba034fab5a4c46b6d0200c053710f3", True),
    ("x", "aabbccddeeff001122334455", "deadbeef00",
     "2041414141414141414141414141414141414141414141414141414141414141"
     "411400000aaabbccddeeff0011223344552c1b610bdf325a7261", True),
]

GCM_SPEC = [
    ("00" * 32, "00" * 12, "", "", "530f8afbc74536b9a963b4f1c4cb738b"),
    ("00" * 32, "00" * 12, "00" * 16, "cea7403d4d606b6e074ec5d3baf39d18",
     "d0d1c8a799996bf0265b98b5d48ab919"),
    ("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
     "cafebabefacedbaddecaf888",
     "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72",
     "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa",
     "35c7d52cb3badf61223e2d2f98ce8ee7"),
]


def selftest():
    fails = 0
    for key, iv, pt, ct, tag in GCM_SPEC:
        got_ct, got_tag = gcm_seal(bytes.fromhex(key), bytes.fromhex(iv),
                                   bytes.fromhex(pt))
        ok = got_ct == bytes.fromhex(ct) and got_tag == bytes.fromhex(tag)
        print(("PASS" if ok else "FAIL") + ": gcm spec vector")
        fails += 0 if ok else 1
    for password, iv, plain, env_hex, reseal in GOLD:
        env = bytes.fromhex(env_hex)
        try:
            _, eff, plain_got = unwrap(password, env)
            ok = plain_got == bytes.fromhex(plain)
        except SystemExit:
            ok = False
        print(("PASS" if ok else "FAIL") + ": golden opens")
        fails += 0 if ok else 1
        if not reseal:
            continue
        lid = env[0]
        raw = int.from_bytes(env[2 + lid:5 + lid], "big")
        rebuilt = wrap(password, env[1:1 + lid],
                       raw * 10000 if raw <= 10000 else raw,
                       bytes.fromhex(plain), bytes.fromhex(iv))
        ok = rebuilt == env
        print(("PASS" if ok else "FAIL") + ": golden reseals byte for byte")
        fails += 0 if ok else 1
    print("ALL PASS" if not fails else "%d FAIL" % fails)
    return fails


def main():
    argv = sys.argv[1:]
    if not argv:
        raise SystemExit(selftest())
    if argv[0] == "wrap" and len(argv) in (5, 6):
        iv = bytes.fromhex(argv[5]) if len(argv) == 6 else None
        env = wrap(argv[1], argv[2].encode(), int(argv[3]),
                   bytes.fromhex(argv[4]), iv)
        print(env.hex())
        return
    if argv[0] == "unwrap" and len(argv) == 3:
        id_, eff, plain = unwrap(argv[1], bytes.fromhex(argv[2]))
        print("id: %s" % (id_.decode("utf-8", "replace") or "(empty)"))
        print("iterations: %d" % eff)
        print("plain: %s" % plain.hex())
        return
    raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
