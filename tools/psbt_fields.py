#!/usr/bin/env python3
"""Dump the per-input key types of a PSBT, so you can see what a coordinator
actually wrote before blaming the signer.

The question this exists to answer: when KISS says "input is not this
wallet's", is that because the input really is foreign, or because the
coordinator left out the field KISS needed to recognise it?

A silent-payment coin is the case where those look identical. Its key is the
wallet's spend key plus a per-output tweak, not a BIP32 child, so the input
carries NO derivation path. KISS proves ownership instead by recomputing the
tweaked key from PSBT_IN_SP_TWEAK (0x20) and matching the P2TR output being
spent. Drop that one field and there is nothing left to match on: the input
falls through to the ordinary keypath check, no keypath bears our
fingerprint, and the refusal is worded as if the coin belonged to someone
else. It doesn't. The metadata is just missing.

Usage:
    python3 tools/psbt_fields.py tx.psbt
    python3 tools/psbt_fields.py cHNidP8B...      # base64 on the command line

Reads binary or base64, from a file or an argument. No dependencies.
"""

import base64
import sys

# BIP174 input key types, plus the silent-payment additions from BIP375/376.
IN_TYPES = {
    0x00: "NON_WITNESS_UTXO",
    0x01: "WITNESS_UTXO",
    0x02: "PARTIAL_SIG",
    0x03: "SIGHASH_TYPE",
    0x04: "REDEEM_SCRIPT",
    0x05: "WITNESS_SCRIPT",
    0x06: "BIP32_DERIVATION",
    0x07: "FINAL_SCRIPTSIG",
    0x08: "FINAL_SCRIPTWITNESS",
    0x0e: "PREVIOUS_TXID",
    0x0f: "OUTPUT_INDEX",
    0x10: "SEQUENCE",
    0x11: "REQUIRED_TIME_LOCKTIME",
    0x12: "REQUIRED_HEIGHT_LOCKTIME",
    0x13: "TAP_KEY_SIG",
    0x14: "TAP_SCRIPT_SIG",
    0x15: "TAP_LEAF_SCRIPT",
    0x16: "TAP_BIP32_DERIVATION",
    0x17: "TAP_INTERNAL_KEY",
    0x18: "TAP_MERKLE_ROOT",
    0x1f: "SP_SPEND_BIP32_DERIVATION",   # BIP376
    0x20: "SP_TWEAK",                    # BIP376 -- the one KISS needs
}

OUT_TYPES = {
    0x00: "REDEEM_SCRIPT",
    0x01: "WITNESS_SCRIPT",
    0x02: "BIP32_DERIVATION",
    0x03: "AMOUNT",
    0x04: "SCRIPT",
    0x05: "TAP_INTERNAL_KEY",
    0x06: "TAP_TREE",
    0x07: "TAP_BIP32_DERIVATION",
    0x09: "SP_V0_INFO",                  # BIP375 recipient, 66B scan||spend
    0x0a: "SP_V0_LABEL",                 # 4B LE label
}


class Reader:
    def __init__(self, buf):
        self.b = buf
        self.i = 0

    def byte(self):
        if self.i >= len(self.b):
            raise EOFError("truncated PSBT")
        v = self.b[self.i]
        self.i += 1
        return v

    def take(self, n):
        if self.i + n > len(self.b):
            raise EOFError("truncated PSBT")
        v = self.b[self.i:self.i + n]
        self.i += n
        return v

    def compact(self):
        n = self.byte()
        if n < 0xfd:
            return n
        width = {0xfd: 2, 0xfe: 4, 0xff: 8}[n]
        return int.from_bytes(self.take(width), "little")

    def keypair(self):
        """Return (keytype, keydata, value), or None at a section terminator."""
        klen = self.compact()
        if klen == 0:
            return None
        key = self.take(klen)
        val = self.take(self.compact())
        return key[0], key[1:], val


def load(arg):
    try:
        with open(arg, "rb") as f:
            raw = f.read()
    except OSError:
        raw = arg.encode()
    if raw[:5] == b"psbt\xff":
        return raw
    # Base64, possibly with whitespace or a trailing newline from a QR dump.
    try:
        dec = base64.b64decode(b"".join(raw.split()), validate=True)
    except Exception:
        raise SystemExit(f"{arg}: not a PSBT (no psbt\\xff magic, not base64)")
    if dec[:5] != b"psbt\xff":
        raise SystemExit(f"{arg}: decoded, but no psbt\\xff magic")
    return dec


def counts(globals_seen, unsigned_tx):
    """How many input and output sections follow the global one.

    Guessing this wrong is not cosmetic: read sections until the buffer runs
    out and the first OUTPUT gets reported as an extra INPUT, which has no
    derivation and no tweak because outputs never do -- exactly the shape of
    the real problem this tool looks for. A false alarm here would send you
    hunting a coordinator bug that isn't there.

    PSBTv2 states the counts in the global section. v0 does not: they live in
    the unsigned transaction, so parse just far enough to reach them.
    """
    if 0x04 in globals_seen and 0x05 in globals_seen:
        return globals_seen[0x04], globals_seen[0x05]
    if unsigned_tx is None:
        return None, None
    t = Reader(unsigned_tx)
    t.take(4)                                          # version
    n_in = t.compact()
    for _ in range(n_in):
        t.take(32 + 4)                                 # prevout
        t.take(t.compact())                            # scriptSig (empty here)
        t.take(4)                                      # sequence
    return n_in, t.compact()


def section(r, table, deriv_key=0x06):
    """Read one key-value section, print its types, return {keytype: value}."""
    seen = {}
    while True:
        kv = r.keypair()
        if kv is None:
            break
        ktype, kdata, val = kv
        name = table.get(ktype, f"UNKNOWN(0x{ktype:02x})")
        seen[ktype] = val
        extra = ""
        if ktype == deriv_key and len(val) >= 4:
            extra = f"  fingerprint {val[:4].hex()}"
        if ktype == 0x20 and table is IN_TYPES:       # SP_TWEAK
            extra = f"  tweak {val.hex()}"
        print(f"    0x{ktype:02x} {name}{extra}")
    if not seen:
        print("    (empty)")
    return seen


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    r = Reader(load(sys.argv[1]))
    r.take(5)                                          # magic

    print("GLOBAL")
    g = section(r, {0x00: "UNSIGNED_TX", 0x01: "XPUB", 0x02: "TX_VERSION",
                    0x03: "FALLBACK_LOCKTIME", 0x04: "INPUT_COUNT",
                    0x05: "OUTPUT_COUNT", 0x06: "TX_MODIFIABLE",
                    0xfb: "VERSION"}, deriv_key=None)

    n_in, n_out = counts({k: int.from_bytes(v, "little") if k in (0x04, 0x05)
                          else v for k, v in g.items()}, g.get(0x00))
    if n_in is None:
        raise SystemExit("cannot determine input count (no INPUT_COUNT, no UNSIGNED_TX)")
    version = "v2" if 0x04 in g else "v0"
    print(f"\nPSBT{version}: {n_in} input(s), {n_out} output(s)\n")

    sp_inputs, plain_inputs = [], []
    for i in range(n_in):
        print(f"INPUT {i}")
        seen = section(r, IN_TYPES)
        if 0x20 in seen:
            sp_inputs.append(i)
        elif 0x06 not in seen and 0x16 not in seen:
            plain_inputs.append(i)

    sp_outs = []
    for i in range(n_out):
        print(f"OUTPUT {i}")
        seen = section(r, OUT_TYPES, deriv_key=0x02)
        if 0x09 in seen:
            sp_outs.append(i)

    print()
    if sp_inputs:
        print(f"silent-payment inputs (carry SP_TWEAK 0x20): {sp_inputs}")
    if sp_outs:
        print(f"silent-payment recipients (carry SP_V0_INFO 0x09): {sp_outs}")
    if plain_inputs:
        print(f"inputs with NO derivation and NO SP_TWEAK: {plain_inputs}")
        print("  ^ KISS cannot prove these are yours and will refuse with")
        print('    "input is not this wallet\'s". If the coin arrived as a')
        print("    silent payment, the coordinator did not write BIP376's")
        print("    SP_TWEAK field -- the coin is yours, the metadata is not there.")
    if not sp_inputs and not plain_inputs and not sp_outs:
        print("every input carries a derivation path; nothing SP-specific here.")


if __name__ == "__main__":
    main()
