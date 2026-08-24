#!/usr/bin/env python3
"""Generate sim/sign_vectors.h for the verifiable-determinism suite.

Offline tool, not run in CI. It reproduces KISS's ECDSA signatures with an
INDEPENDENT implementation (embit), so the golden vectors the C test asserts are
not merely KISS's own output pasted back. embit's low-R signing grinds the
RFC6979 nonce with an incrementing 32-byte little-endian counter until the DER
signature is <= 70 bytes -- byte-for-byte the convention libwally's
EC_FLAG_GRIND_R uses, which is why the two agree.

The unsigned PSBTs below are the fixed fixtures sim/test_crypto.c builds with
mk_typed_psbt() for the dev seed (mnemonic "abandon ... about", fp 73C5DA0A).
They are public input data; only the signature is what this tool computes
independently. If mk_typed_psbt ever changes a fixture, refresh the hex here
(dump it from the test) and re-run.

Setup:
  python3 -m venv venv && ./venv/bin/pip install embit
Usage:
  ./venv/bin/python tools/sign_fixtures/gen_sign_vectors.py

Paste the printed lines into sim/sign_vectors.h. Do NOT redirect over that file:
it also carries the SV_SCHNORR_SP_* block, which comes from the sibling
gen_sp_sign_vectors.py and this script does not print.

Schnorr (silent-payment) vectors are not produced here: they need the taproot
sighash from main/sp_spend_vectors.h and the SP-capable embit fork that
gen_spend_vectors.py already depends on. Those are plain BIP340 with
aux_rand = 0. See docs/specs/verifiable-determinism.md.
"""

import sys

try:
    from embit import bip39, bip32
    from embit.psbt import PSBT
except ImportError:
    sys.exit("need embit: python3 -m venv venv && ./venv/bin/pip install embit")

MNEMONIC = ("abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon abandon abandon about")

# (macro suffix, unsigned PSBT hex) for the fixed legacy/nested/native fixtures.
FIXTURES = {
    "LEGACY": "70736274ff0100740200000001cc1c7c16a8c2e4f028e5f2f3e68958d6c38005781a77fc5b33a84122f3090ad90000000000fdffffff0260ea000000000000160014111111111111111111111111111111111111111158980000000000001976a914bae93c8e7fb682422d24780b1a12a550eff428f288ac00000000000100550200000001bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb0000000000ffffffff01a0860100000000001976a914d986ed01b7a22225a70edbf2ba7cfb63a15cb3aa88ac00000000220603aaeb52dd7494c361049de67cc680e83ebcbbbdbeb13637d92cd845f70308af5e1873c5da0a2c000080000000800000008000000000000000000000220203498b3ac8e882c5d693540c49adf22b7a1b99c1bb8047966739bfe8cdeb272e641873c5da0a2c0000800000008000000080010000000000000000",
    "NESTED": "70736274ff0100720200000001aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa0000000000fdffffff0260ea0000000000001600141111111111111111111111111111111111111111589800000000000017a9141cc1e09a63d1ae795a7130e099b28a0b1d8e4fae870000000000010120a08601000000000017a9143fb6e95812e57bb4691f9a4a628862a61a4f769b870104160014f990679acafe25c27615373b40bf22446d24ff442206039b3b694b8fc5b5e07fb069c783cac754f5d38c3e08bed1960e31fdb1dda35c241873c5da0a31000080000000800000008000000000000000000000220202b4019c64bb1347bd729a6afa11348bd80be4ebc314df03f654f786bfe2b4a7281873c5da0a310000800000008000000080010000000000000000",
    "NATIVE": "70736274ff0100710200000001aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa0000000000fdffffff0260ea000000000000160014111111111111111111111111111111111111111158980000000000001600143e34985dca6fddc9fb369940e4c7d8e2873f529c000000000001011fa086010000000000160014c0cebcd6c3d3ca8c75dc5ec62ebe55330ef910e222060330d54fd0dd420a6e5f8d3624f5f3482cae350f79d5f0753bf5beef9c2d91af3c1873c5da0a54000080000000800000008000000000000000000000220203025324888e429ab8e3dbaf1f7802648b9cd01e9b418485c5fa4c1b9b5700e1a61873c5da0a540000800000008000000080010000000000000000",
}

HEADER = """// GENERATED, in two halves, by tools/sign_fixtures/gen_sign_vectors.py (the
// SV_ECDSA_* block) and gen_sp_sign_vectors.py (the SV_SCHNORR_SP_* block).
// Do not hand-edit, and do not redirect either generator over this file --
// neither prints the other's half.
//
// Golden signatures for the verifiable-determinism suite. Each is the exact
// byte string (DER signature + trailing sighash byte, lower-case hex) that an
// INDEPENDENT signer -- embit's low-R ECDSA, which grinds the RFC6979 nonce the
// same way libwally's EC_FLAG_GRIND_R does -- produces for the fixed dev seed
// (mnemonic "abandon abandon ... about", fingerprint 73C5DA0A) over the fixed
// test PSBTs in sim/test_crypto.c.
//
// These are NOT copied from KISS's own output. gen_sign_vectors.py signs the
// same PSBTs with embit and prints these lines; the C test signs them with the
// device code and asserts equality. If KISS's nonce derivation ever drifts, the
// two disagree and the build fails. See docs/specs/verifiable-determinism.md.
#pragma once
"""


def main():
    root = bip32.HDKey.from_seed(bip39.mnemonic_to_seed(MNEMONIC))
    print(HEADER)
    for name, unsigned in FIXTURES.items():
        psbt = PSBT.parse(bytes.fromhex(unsigned))
        psbt.sign_with(root)
        sig = list(psbt.inputs[0].partial_sigs.values())[0].hex()
        print(f'#define SV_ECDSA_{name} "{sig}"')


if __name__ == "__main__":
    main()
