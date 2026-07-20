#!/usr/bin/env python3
"""Generate main/sp_test_vectors.h for the kisstest silent-payments suite.

Offline tool: needs two checkouts on disk, neither vendored nor used in CI.

  --bips   clone of https://github.com/bitcoin/bips (sparse: bip-0352 bip-0374)
  --embit  path to the SP-capable embit fork's src/ directory, commit cf085f8
           (the submodule of selfcustody/krux PR #870: vendor/embit/src)

Everything the C tests assert flows from here: BIP352 send vectors verbatim
from the bips repo, BIP374 DLEQ vectors verbatim from the bips CSVs, and a
coordinator-style PSBTv2 fixture (plus negative variants) built with the embit
fork, mirroring Krux's test_psbt_silent_payments fixture so our signer can be
cross-checked against a second implementation.

Usage:
  python3 tools/sp_fixtures/gen_vectors.py --bips ~/src/bips --embit /path/embit/src
"""

import argparse
import csv
import json
import subprocess
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parents[2] / "main" / "sp_test_vectors.h"

# Anchors shared with the embit fork's own test suite (test_psbt_silent_payments.py)
TEST_MNEMONIC = (
    "abandon abandon abandon abandon abandon abandon abandon abandon "
    "abandon abandon abandon about"
)
SCAN_HEX = "027a487fc19fb769877b8742d6ea18118f3c4e72b1ea8c6de602a7ad4a41dbe068"
SPEND_HEX = "0361e1b1e9de5e42cb2007f7ca54b9e0d57ed13938fad56d3f19e57513a8fce039"
INPUT_PATH = [84 + 2**31, 1 + 2**31, 0 + 2**31, 0, 0]
CHANGE_PATH = [84 + 2**31, 1 + 2**31, 0 + 2**31, 1, 0]

# BIP352 sending vectors to embed, picked by their json comment (stable ids
# upstream). Coverage: plain multi-input, taproot inputs (x-only negation),
# multiple outputs to one scan key (k ordering).
WANTED_COMMENTS = [
    "Simple send: two inputs",
    "Single recipient: taproot input with odd y-value and non-taproot input",
    "Multiple outputs: multiple outputs, multiple recipients",
]


def chex(b):
    return ", ".join("0x%02x" % x for x in b)


def carr(name, b):
    return "static const uint8_t %s[%d] = { %s };\n" % (name, len(b), chex(b))


def cstr(name, s):
    return 'static const char %s[] = "%s";\n' % (name, s)


def bips_vectors(bips):
    vecs = json.load(open(bips / "bip-0352" / "send_and_receive_test_vectors.json"))
    picked = []
    for want in WANTED_COMMENTS:
        hit = [v for v in vecs if v["comment"] == want]
        if not hit:
            sys.exit("BIP352 vector with comment %r not found" % want)
        picked.append(hit[0])
    return picked


def emit_bip352(out, picked, embit_mods):
    ec, script_mod, COutPoint = embit_mods
    out.append("// ---- BIP352 sending vectors (verbatim from bips repo) ----\n")
    out.append("#define SPV_BIP352_N %d\n" % len(picked))
    for vi, vec in enumerate(picked):
        send = vec["sending"][0]
        given, expected = send["given"], send["expected"]
        vins = given["vin"]
        privs, xonly_flags, outpoints = [], [], b""
        for vin in vins:
            privs.append(bytes.fromhex(vin["private_key"]))
            spk = bytes.fromhex(vin["prevout"]["scriptPubKey"]["hex"])
            is_p2tr = len(spk) == 34 and spk[0] == 0x51 and spk[1] == 0x20
            xonly_flags.append(is_p2tr)
            # NB: the json's txid hex is already in serialization byte order
            # (verified against the reference: reversing it breaks the vectors)
            outpoints += COutPoint(bytes.fromhex(vin["txid"]), vin["vout"]).serialize()
        # one output per recipient ENTRY (duplicates kept, given order). The
        # json's expected.outputs is a list of ACCEPTABLE OUTPUT SETS: k
        # assignment depends on recipient ordering, so several sets can be
        # valid. Emit them all; the C test multiset-matches against any one.
        from embit.silent_payments.bip352 import decode_silent_payment_address

        recips = [r if isinstance(r, str) else r["address"] for r in given["recipients"]]
        keyblob = b""
        for addr in recips:
            B_scan, B_spend = decode_silent_payment_address(addr)
            keyblob += B_scan.sec() + B_spend.sec()
        alts = expected["outputs"]
        for a in alts:
            if len(a) != len(recips):
                sys.exit("vector %r: alternative size != recipient count" % vec["comment"])
        out.append("// vector %d: %s\n" % (vi, vec["comment"]))
        out.append("#define SPV352_%d_NIN %d\n" % (vi, len(vins)))
        out.append("#define SPV352_%d_NOUT %d\n" % (vi, len(recips)))
        out.append("#define SPV352_%d_NALT %d\n" % (vi, len(alts)))
        out.append(carr("spv352_%d_privs" % vi, b"".join(privs)))
        out.append(
            "static const uint8_t spv352_%d_xonly[%d] = { %s };\n"
            % (vi, len(vins), ", ".join("1" if f else "0" for f in xonly_flags))
        )
        out.append(carr("spv352_%d_outpoints" % vi, outpoints))
        out.append(
            carr("spv352_%d_asum" % vi, bytes.fromhex(expected["input_private_key_sum"]))
        )
        out.append(carr("spv352_%d_recipkeys" % vi, keyblob))
        out.append(
            carr("spv352_%d_expect" % vi,
                 b"".join(bytes.fromhex(x) for a in alts for x in a))
        )


def emit_dleq(out, bips):
    gen_rows = list(csv.DictReader(open(bips / "bip-0374" / "test_vectors_generate_proof.csv")))
    ver_rows = list(csv.DictReader(open(bips / "bip-0374" / "test_vectors_verify_proof.csv")))
    out.append("// ---- BIP374 DLEQ vectors (verbatim from bips repo CSVs) ----\n")
    gp = [r for r in gen_rows if r["result_proof"] and not r["result_proof"].startswith("INVALID")]
    out.append("#define SPV_DLEQ_GEN_N %d\n" % len(gp))
    for i, r in enumerate(gp):
        out.append("// gen %d: %s\n" % (i, r["comment"]))
        out.append(carr("spvdg_%d_G" % i, bytes.fromhex(r["point_G"])))
        out.append(carr("spvdg_%d_a" % i, bytes.fromhex(r["scalar_a"])))
        out.append(carr("spvdg_%d_B" % i, bytes.fromhex(r["point_B"])))
        out.append(carr("spvdg_%d_aux" % i, bytes.fromhex(r["auxrand_r"])))
        out.append(carr("spvdg_%d_msg" % i, bytes.fromhex(r["message"])))
        out.append(carr("spvdg_%d_proof" % i, bytes.fromhex(r["result_proof"])))
    out.append("#define SPV_DLEQ_VER_N %d\n" % len(ver_rows))
    for i, r in enumerate(ver_rows):
        out.append("// verify %d: %s -> %s\n" % (i, r["comment"], r["result_success"]))
        out.append(carr("spvdv_%d_G" % i, bytes.fromhex(r["point_G"])))
        out.append(carr("spvdv_%d_A" % i, bytes.fromhex(r["point_A"])))
        out.append(carr("spvdv_%d_B" % i, bytes.fromhex(r["point_B"])))
        out.append(carr("spvdv_%d_C" % i, bytes.fromhex(r["point_C"])))
        out.append(carr("spvdv_%d_proof" % i, bytes.fromhex(r["proof"])))
        out.append(carr("spvdv_%d_msg" % i, bytes.fromhex(r["message"])))
        out.append(
            "static const int spvdv_%d_ok = %d;\n"
            % (i, 1 if r["result_success"] == "TRUE" else 0)
        )


def build_fixtures(out):
    from embit import bip32, bip39, ec, script, bech32
    from embit.psbt import DerivationPath
    from embit.transaction import TransactionOutput, COutPoint
    from embit.networks import NETWORKS
    from embit.silent_payments import SilentPaymentsPSBT, create_outputs
    from embit.silent_payments.psbt import SPInputScope, SPOutputScope
    from embit.silent_payments.fields import SilentPaymentData
    from embit.silent_payments.ecdh import compute_ecdh_share, compute_dleq_proof
    from embit.silent_payments.bip352 import get_input_hash

    root = bip32.HDKey.from_seed(bip39.mnemonic_to_seed(TEST_MNEMONIC))
    child = root.derive(INPUT_PATH)
    change = root.derive(CHANGE_PATH)
    pub = child.get_public_key()
    scan_pub = ec.PublicKey.parse(bytes.fromhex(SCAN_HEX))
    spend_pub = ec.PublicKey.parse(bytes.fromhex(SPEND_HEX))

    # sp address (tsp) exactly as Krux encodes it
    payload = scan_pub.sec() + spend_pub.sec()
    data = bech32.convertbits(payload, 8, 5)
    address = bech32.bech32_encode(bech32.Encoding.BECH32M, "tsp", [0] + data)

    def base_psbt():
        psbt = SilentPaymentsPSBT.create_v2()
        inp = SPInputScope()
        inp.txid = bytes([0xAB] * 32)
        inp.vout = 0
        inp.sequence = 0xFFFFFFFE
        inp.witness_utxo = TransactionOutput(
            value=120_000, script_pubkey=script.p2wpkh(pub)
        )
        inp.bip32_derivations[pub] = DerivationPath(root.my_fingerprint, INPUT_PATH)
        psbt.add_input(inp)
        spo = SPOutputScope()
        spo.value = 95_000
        spo.script_pubkey = None  # coordinator omits: the signer must derive it
        spo.sp_data = SilentPaymentData(scan_pub, spend_pub)
        psbt.add_output(spo)
        cho = SPOutputScope()
        cho.value = 20_000
        cho.script_pubkey = script.p2wpkh(change.get_public_key())
        cho.bip32_derivations[change.get_public_key()] = DerivationPath(
            root.my_fingerprint, CHANGE_PATH
        )
        psbt.add_output(cho)
        psbt.tx_modifiable_flags = 0
        return psbt

    out.append("// ---- coordinator-style PSBTv2 fixture (embit fork cf085f8) ----\n")
    out.append(cstr("SPV_PSBT_B64", base_psbt().to_string()))
    out.append(cstr("SPV_ADDR_EXPECT", address))
    out.append(carr("SPV_ADDR_SCAN", scan_pub.sec()))
    out.append(carr("SPV_ADDR_SPEND", spend_pub.sec()))
    out.append(cstr("SPV_MASTER_XPRV", root.to_base58(version=NETWORKS["test"]["xprv"])))
    out.append(carr("SPV_FP", root.my_fingerprint))

    # expected derivation, computed the create_outputs way (independent path)
    outpoints = [COutPoint(bytes([0xAB] * 32), 0)]
    outputs_map = create_outputs([(child.key.secret, False)], outpoints, [address])
    xonly = bytes.fromhex(outputs_map[address][0])
    out.append(carr("SPV_PSBT_EXPECT_SCRIPT", b"\x51\x20" + xonly))
    share = compute_ecdh_share(child.key.secret, scan_pub)
    out.append(carr("SPV_PSBT_EXPECT_SHARE", share))
    a_sum_pub = ec.PrivateKey(child.key.secret).get_public_key()
    out.append(carr("SPV_PSBT_ASUM_PUB", a_sum_pub.sec()))
    out.append(carr("SPV_PSBT_INPUT_HASH", get_input_hash(outpoints, a_sum_pub.sec())))

    # variant: coordinator already added a (valid) per-input share + proof;
    # the signer must wipe and recompute, load stays READY
    v = base_psbt()
    proof = compute_dleq_proof(child.key.secret, scan_pub, share, aux_rand=bytes(32))
    v.inputs[0].sp_ecdh_shares[scan_pub.sec()] = share
    v.inputs[0].sp_dleq_proofs[scan_pub.sec()] = proof
    out.append(cstr("SPV_PSBT_FOREIGN_SHARE_B64", v.to_string()))

    # variant: sighash SINGLE on the input -> STOP
    v = base_psbt()
    v.inputs[0].sighash_type = 3
    out.append(cstr("SPV_PSBT_SIGHASH_B64", v.to_string()))

    # variant: BIP376 receive-side field present -> STOP
    v = base_psbt()
    v.inputs[0].sp_tweak = bytes(range(32))
    out.append(cstr("SPV_PSBT_BIP376_B64", v.to_string()))

    # variant: PSBTv0 carrying a 0x09 SP INFO output unknown -> STOP
    from embit.psbt import PSBT as PSBTv0
    from embit.transaction import Transaction, TransactionInput

    tx = Transaction(
        vin=[TransactionInput(bytes([0xAB] * 32), 0, sequence=0xFFFFFFFE)],
        vout=[TransactionOutput(95_000, script.Script(b""))],
    )
    p0 = PSBTv0(tx)
    p0.inputs[0].witness_utxo = TransactionOutput(
        value=120_000, script_pubkey=script.p2wpkh(pub)
    )
    p0.inputs[0].bip32_derivations[pub] = DerivationPath(root.my_fingerprint, INPUT_PATH)
    p0.outputs[0].unknown[b"\x09"] = scan_pub.sec() + spend_pub.sec()
    out.append(cstr("SPV_PSBT_V0_SP_B64", p0.to_string()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bips", required=True, type=Path)
    ap.add_argument("--embit", required=True, type=Path)
    args = ap.parse_args()
    sys.path.insert(0, str(args.embit))

    import embit  # noqa: F401  (fail fast if the fork isn't importable)
    from embit import ec, script
    from embit.transaction import COutPoint

    if not hasattr(__import__("embit.silent_payments", fromlist=["x"]), "create_outputs"):
        sys.exit("embit at --embit has no silent_payments module (need the SP fork)")

    bips_rev = subprocess.run(
        ["git", "-C", str(args.bips), "rev-parse", "--short", "HEAD"],
        capture_output=True, text=True,
    ).stdout.strip()

    out = []
    out.append("// generated by tools/sp_fixtures/gen_vectors.py -- do not hand-edit\n")
    out.append("// sources: bips@%s (bip-0352 json, bip-0374 csv, verbatim),\n" % bips_rev)
    out.append("//          embit SP fork cf085f8 (PSBTv2 fixtures + expected values)\n")
    out.append("#pragma once\n#include <stdint.h>\n\n")
    emit_bip352(out, bips_vectors(args.bips), (ec, script, COutPoint))
    out.append("\n")
    emit_dleq(out, args.bips)
    out.append("\n")
    build_fixtures(out)
    OUT.write_text("".join(out))
    print("wrote %s (%d lines)" % (OUT, "".join(out).count("\n")))


if __name__ == "__main__":
    main()
