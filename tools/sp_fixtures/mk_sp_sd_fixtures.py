#!/usr/bin/env python3
"""Emit real BIP375 silent-payment PSBTv2 fixtures for on-device SD testing
(testnet keypaths: m/84'/1'/0'/0/i inputs, change m/84'/1'/0'/1/0).

Built on the SP-capable embit fork (diybitcoinhardware/embit commit cf085f8,
the krux PR #870 vendor/embit submodule) -- the exact tool that produced the
golden SPV_PSBT_B64 in main/sp_test_vectors.h. Every emitted file is captured
through that same serializer, and the tool refuses to write anything unless it
reproduces the golden 1-in fixture byte-for-byte first.

The files are RAW PSBTv2 binaries; kiss_psbt_load accepts raw bytes from the
SD path directly. Scan/spend recipient keys and the input/change keypaths are
the same constants the golden fixture uses, so every emitted file carries the
same BIP352 recipient and the dev-seed owner.

Amounts keep the golden ratio: each input 120000 sats, SP output 950000 (10-in)
/ 1900000 (20-in), change 200000 / 400000, fee exactly 5.26% in both, so no
caution row gets in the way of the shape under test.

Setup: the fork must be reachable -- pass --embit <path-to-src> or set it so
it resolves to the checked-out krux vendor/embit/src at cf085f8.
"""
import argparse
import base64
import sys
from pathlib import Path

GOLDEN_B64 = (
    "cHNidP8BAgQCAAAAAQQBAQEFAQIBBgEAAfsEAgAAAAABAR/A1AEAAAAAABYAFNDEo+8J6Ze26Z45"
    "flGP4+QaEYyhIgYC56slN7XUnpcDCargbp5J82zhyf671E7I4NHMoLT5wxkYc8XaClQAAIABAACA"
    "AAAAgAAAAAAAAAAAAQ4gq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6sBDwQAAAAAARAE"
    "/v///wABAwgYcwEAAAAAAAEJQgJ6SH/Bn7dph3uHQtbqGBGPPE5yseqMbeYCp61KQdvgaANh4bHp"
    "3l5CyyAH98pUueDVftE5OPrVbT8Z5XUTqPzgOQAiAgNdSezNVNAJnkNnYnfHptRiXWEdqIpd9Jv5"
    "UXp3kad3pRhzxdoKVAAAgAEAAIAAAACAAQAAAAAAAAABAwggTgAAAAAAAAEEFgAULzSqHPAKU7BV"
    "opGgOn1F8KaYi1IA"
)

MNEMONIC = (
    "abandon abandon abandon abandon abandon abandon abandon abandon "
    "abandon abandon abandon about"
)
# Testnet coin slot (m/84'/1'/0'): same keypaths the golden fixture uses.
IN_PATH = "m/84h/1h/0h/0/%d"
CHG_PATH = "m/84h/1h/0h/1/0"
SCAN_HEX = "027a487fc19fb769877b8742d6ea18118f3c4e72b1ea8c6de602a7ad4a41dbe068"
SPEND_HEX = "0361e1b1e9de5e42cb2007f7ca54b9e0d57ed13938fad56d3f19e57513a8fce039"

VALUE_PER_IN = 120000
SEQUENCE = 0xFFFFFFFE


def build(n_in, sp_val, change_val, first_txid_byte):
    global fp, PSBT, SPInputScope, SPOutputScope, SilentPaymentData
    from embit.bip32 import parse_path
    from embit.psbt import DerivationPath
    from embit.script import p2wpkh
    from embit.transaction import TransactionOutput

    root_path = parse_path

    def deriv(path):
        k = root.derive(path)
        return k.get_public_key()

    psbt = PSBT.create_v2()
    for i in range(n_in):
        pub = deriv(IN_PATH % i)
        inp = SPInputScope()
        inp.txid = bytes([first_txid_byte + i]) * 32
        inp.vout = 0
        inp.sequence = SEQUENCE
        inp.witness_utxo = TransactionOutput(
            value=VALUE_PER_IN, script_pubkey=p2wpkh(pub))
        inp.bip32_derivations[pub] = DerivationPath(
            fp, parse_path(IN_PATH % i))
        psbt.add_input(inp)

    sp_out = SPOutputScope()
    sp_out.value = sp_val
    sp_out.sp_data = SilentPaymentData(
        ec.PublicKey.parse(bytes.fromhex(SCAN_HEX)),
        ec.PublicKey.parse(bytes.fromhex(SPEND_HEX)))
    psbt.add_output(sp_out)

    chg_pub = deriv(CHG_PATH)
    chg = SPOutputScope()
    chg.value = change_val
    chg.script_pubkey = p2wpkh(chg_pub)
    chg.bip32_derivations[chg_pub] = DerivationPath(
        fp, parse_path(CHG_PATH))
    psbt.add_output(chg)
    psbt.tx_modifiable_flags = 0
    return psbt.serialize()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--embit", required=True,
                    help="embit SP fork src/ (commit cf085f8)")
    ap.add_argument("outdir", nargs="?", default=".")
    args = ap.parse_args()
    sys.path.insert(0, str(Path(args.embit).resolve()))

    global fp, PSBT, SPInputScope, SPOutputScope, SilentPaymentData, ec, root
    from embit import bip32, bip39, ec
    from embit.silent_payments import SilentPaymentsPSBT as PSBT
    from embit.silent_payments.psbt import SPInputScope, SPOutputScope
    from embit.silent_payments.fields import SilentPaymentData

    root = bip32.HDKey.from_seed(bip39.mnemonic_to_seed(MNEMONIC))
    fp = root.my_fingerprint
    want_fp = bytes.fromhex("73c5da0a")
    if fp != want_fp:
        sys.exit("master fingerprint mismatch: %s" % fp.hex())

    one = build(1, 95000, 20000, 0xAB)
    golden = base64.b64decode(GOLDEN_B64)
    if one != golden:
        sys.exit("1-in rebuild differs from golden fixture — refusing to write")
    print("golden fixture reproduced byte-for-byte")

    fixtures = [
        ("11-sp-10in.psbt", build(10, 950000, 200000, 0xB0)),
        ("12-sp-20in.psbt", build(20, 1900000, 400000, 0xB0)),
    ]
    d = Path(args.outdir)
    d.mkdir(parents=True, exist_ok=True)
    for name, data in fixtures:
        (d / name).write_bytes(data)
        print("wrote %-18s %5d bytes  (SP send, %d in, 2 out)" %
              (name, len(data), 10 if name.startswith("11") else 20))


if __name__ == "__main__":
    main()