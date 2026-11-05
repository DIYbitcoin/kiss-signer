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

Shared sends (a silent payment two signers pay together, BIP375 per input
shares) have their own header, sim/sp_shared_vectors.h, and their own mode:

  python3 tools/sp_fixtures/gen_vectors.py --shared --embit /path/embit/src \
      [--bip375 /path/bip375_test_vectors.json]

It needs no bips checkout and leaves main/sp_test_vectors.h alone. It is
written against the embit that Krux vendors at e89d799, whose silent payment
modules are named differently from cf085f8 above (silent_payments.sp, .psbt
and .dleq), so the two modes do not share an embit. --embit may be left out
when embit is already importable (PYTHONPATH). --bip375 adds the per input
share cases of the BIP375 test vector file, which a third implementation
wrote; without it that block is left out and the C tests that read it skip.
"""

import argparse
import csv
import json
import subprocess
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parents[2] / "main" / "sp_test_vectors.h"
OUT_SHARED = Path(__file__).resolve().parents[2] / "sim" / "sp_shared_vectors.h"

# The second wallet of a shared send: the seed the Krux test bridge defaults
# to, so the fixtures here are the two wallets the three codebases meet with.
MNEMONIC_B = (
    "legal winner thank year wave sausage worth useful legal winner thank yellow"
)
# BIP375 test vectors that carry per input shares, by description.
BIP375_PER_INPUT = "can finalize: two inputs single-signer using per-input ECDH shares"
BIP375_BAD_PROOF = "ecdh coverage: invalid proof in PSBT_IN_SP_DLEQ field"

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


def carr_or_null(name, b):
    """32-byte array, or a NULL pointer when the vector's field is empty
    (BIP374 messages are optional; C code takes NULL for 'no message')."""
    if not b:
        return "static const uint8_t *const %s = NULL;\n" % name
    return carr(name, b)


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
        out.append(carr_or_null("spvdg_%d_msg" % i, bytes.fromhex(r["message"])))
        out.append(carr("spvdg_%d_proof" % i, bytes.fromhex(r["result_proof"])))
    out.append("#define SPV_DLEQ_VER_N %d\n" % len(ver_rows))
    for i, r in enumerate(ver_rows):
        out.append("// verify %d: %s -> %s\n" % (i, r["comment"], r["result_success"]))
        out.append(carr("spvdv_%d_G" % i, bytes.fromhex(r["point_G"])))
        out.append(carr("spvdv_%d_A" % i, bytes.fromhex(r["point_A"])))
        out.append(carr("spvdv_%d_B" % i, bytes.fromhex(r["point_B"])))
        out.append(carr("spvdv_%d_C" % i, bytes.fromhex(r["point_C"])))
        out.append(carr("spvdv_%d_proof" % i, bytes.fromhex(r["proof"])))
        out.append(carr_or_null("spvdv_%d_msg" % i, bytes.fromhex(r["message"])))
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


def build_shared(out, bip375):
    """Fixtures for a silent payment paid by two wallets, each with its own
    signer: wallet A is the KISS dev seed, wallet B the Krux bridge seed.

    Everything the C tests compare against is computed here with embit, on a
    route the signer does not take: the output script from the SUM OF THE
    PRIVATE KEYS (the single signer derivation), where the signer under test
    has to reach it from the sum of the per input shares.
    """
    import hashlib

    from embit import bip32, bip39, ec, script
    from embit.psbt import DerivationPath
    from embit.transaction import Transaction, TransactionInput, TransactionOutput
    from embit.silent_payments import SilentPaymentsPSBT, dleq
    from embit.silent_payments.psbt import (
        SPInputScope,
        SPOutputScope,
        SilentPaymentData,
    )
    from embit.silent_payments.sp import (
        derive_sp_outputs,
        group_sp_outputs_by_scan_key,
    )
    from embit.util.secp256k1 import (
        ec_pubkey_combine,
        ec_pubkey_parse,
        ec_pubkey_serialize,
        ec_pubkey_tweak_mul,
    )
    from embit.util.key import SECP256K1_ORDER

    scan_pub = ec.PublicKey.parse(bytes.fromhex(SCAN_HEX))
    spend_pub = ec.PublicKey.parse(bytes.fromhex(SPEND_HEX))
    scan = scan_pub.sec()

    class Wallet:
        def __init__(self, mnemonic):
            self.root = bip32.HDKey.from_seed(bip39.mnemonic_to_seed(mnemonic))
            self.fp = self.root.my_fingerprint
            self.coin = self.root.derive(INPUT_PATH)
            self.change = self.root.derive(CHANGE_PATH)
            self.pub = self.coin.get_public_key()
            self.secret = self.coin.key.secret

        def kiss_aux(self, psbt_bytes):
            """The DLEQ aux this firmware derives (kiss_psbt.c): the same PSBT
            and the same wallet give the same proof, so a proof can be pinned."""
            return hashlib.sha256(
                self.root.key.secret + self.fp + hashlib.sha256(psbt_bytes).digest()
            ).digest()

    A = Wallet(TEST_MNEMONIC)
    B = Wallet(MNEMONIC_B)

    def share_of(secret):
        point = bytearray(ec_pubkey_parse(scan))
        ec_pubkey_tweak_mul(point, secret)
        return ec_pubkey_serialize(point)

    def proof_of(secret, aux):
        return dleq.generate_dleq_proof(secret, scan, r=aux)

    def prev_tx(marker, outs):
        """A previous transaction paying `outs`, so the input that spends it
        carries the whole of it and its amount is proven by its txid."""
        return Transaction(
            version=2,
            vin=[TransactionInput(bytes([marker] * 32), 0)],
            vout=[TransactionOutput(v, spk) for v, spk in outs],
        )

    stranger = script.p2wpkh(ec.PrivateKey(bytes([0x5A] * 32)).get_public_key())
    IN_A, IN_B, PAY, CHANGE_A, CHANGE_B = 60_000, 50_000, 20_000, 49_800, 39_800
    prev_a = prev_tx(0xA1, [(IN_A, script.p2wpkh(A.pub))])
    # B's coin is the SECOND output of its transaction, so an outpoint index
    # that is not zero is part of every fixture.
    prev_b = prev_tx(0xB2, [(7_000, stranger), (IN_B, script.p2wpkh(B.pub))])

    def wpkh_input(wallet, prev, vout, with_prev=True, with_key=True):
        inp = SPInputScope()
        inp.txid = prev.txid()
        inp.vout = vout
        inp.sequence = 0xFFFFFFFD
        inp.witness_utxo = prev.vout[vout]
        if with_prev:
            inp.non_witness_utxo = prev
        if with_key:
            inp.bip32_derivations[wallet.pub] = DerivationPath(wallet.fp, INPUT_PATH)
        return inp

    def sp_output(value=PAY, spk=None):
        o = SPOutputScope()
        o.value = value
        o.script_pubkey = spk
        o.sp_data = SilentPaymentData(scan_pub, spend_pub)
        return o

    def change_output(wallet, value):
        o = SPOutputScope()
        o.value = value
        o.script_pubkey = script.p2wpkh(wallet.change.get_public_key())
        o.bip32_derivations[wallet.change.get_public_key()] = DerivationPath(
            wallet.fp, CHANGE_PATH
        )
        return o

    def assemble(inputs, outputs):
        psbt = SilentPaymentsPSBT.create_v2()
        for i in inputs:
            psbt.add_input(i)
        for o in outputs:
            psbt.add_output(o)
        psbt.tx_modifiable_flags = 0
        return psbt

    def base(in_b=None, sp_spk=None):
        return assemble(
            [wpkh_input(A, prev_a, 0), in_b or wpkh_input(B, prev_b, 1)],
            [sp_output(spk=sp_spk), change_output(A, CHANGE_A), change_output(B, CHANGE_B)],
        )

    def put_share(psbt, i, share=None, proof=None):
        if share is not None:
            psbt.inputs[i].unknown[b"\x1d" + scan] = share
        if proof is not None:
            psbt.inputs[i].unknown[b"\x1e" + scan] = proof
        return psbt

    def expected_script(secrets, psbt):
        groups, _ = group_sp_outputs_by_scan_key(psbt.outputs)
        a_sum, a_sum_pub, results = derive_sp_outputs(
            secrets, [i.vin for i in psbt.inputs], groups
        )
        share_sum, xonly = results[scan]
        return b"\x51\x20" + xonly[0], a_sum_pub, share_sum

    def point_sum(points):
        return ec_pubkey_serialize(ec_pubkey_combine(*[ec_pubkey_parse(p) for p in points]))

    share_a, share_b = share_of(A.secret), share_of(B.secret)
    proof_b = proof_of(B.secret, bytes(32))
    proof_a = proof_of(A.secret, bytes([1] * 32))
    spk, a_sum_pub, share_sum = expected_script([A.secret, B.secret], base())
    # The two routes agree before anything is written: the shares add up to
    # the share one signer holding both keys would have made.
    assert point_sum([share_a, share_b]) == share_sum
    assert point_sum([A.pub.sec(), B.pub.sec()]) == a_sum_pub
    assert dleq.verify_dleq_proof(B.pub.sec(), scan, share_b, proof_b)

    r0 = base()
    r_b = put_share(base(), 1, share_b, proof_b)        # B's signer went first
    r_a = put_share(base(), 0, share_a, proof_a)        # A's signer went first
    r_ab = put_share(put_share(base(sp_spk=script.Script(spk)), 0, share_a, proof_a),
                     1, share_b, proof_b)

    out.append("// generated by tools/sp_fixtures/gen_vectors.py --shared, do not hand-edit\n")
    out.append("// source: embit as vendored by Krux at e89d799 (PSBTv2 fixtures and every\n")
    out.append("//         expected value)%s\n"
               % (", bip375_test_vectors.json (per input share cases)" if bip375 else ""))
    out.append("#pragma once\n#include <stdint.h>\n\n")
    out.append("// ---- a silent payment two wallets pay together (BIP375 per input shares) ----\n")
    out.append("// wallet A: the KISS dev seed. wallet B: the Krux bridge seed, below.\n")
    out.append("// input 0 is A's (m/84'/1'/0'/0/0), input 1 is B's (same path, B's seed).\n")
    out.append("// output 0 the silent payment, output 1 A's change, output 2 B's change.\n")
    out.append(cstr("SPS_MNEMONIC_B", MNEMONIC_B))
    for name, v in (("SPS_IN_A", IN_A), ("SPS_IN_B", IN_B), ("SPS_PAY", PAY),
                    ("SPS_CHANGE_A", CHANGE_A), ("SPS_CHANGE_B", CHANGE_B),
                    ("SPS_FEE", IN_A + IN_B - PAY - CHANGE_A - CHANGE_B)):
        out.append("#define %s %dULL\n" % (name, v))
    out.append(carr("SPS_SCAN", scan))
    out.append(carr("SPS_SPEND", spend_pub.sec()))
    out.append(carr("SPS_A_PUB", A.pub.sec()))
    out.append(carr("SPS_B_PUB", B.pub.sec()))
    out.append("// each input's share a_i*B_scan, by embit\n")
    out.append(carr("SPS_A_SHARE", share_a))
    out.append(carr("SPS_B_SHARE", share_b))
    out.append("// the output script, from the SUM OF BOTH PRIVATE KEYS (derive_sp_outputs)\n")
    out.append(carr("SPS_SCRIPT", spk))
    out.append(carr("SPS_ASUM_PUB", a_sum_pub))

    out.append("// request with no share yet: either signer makes a share only pass\n")
    out.append(cstr("SPS_R0_B64", r0.to_string()))
    out.append("// ...B's signer went first: A's pass completes the set\n")
    out.append(cstr("SPS_RB_B64", r_b.to_string()))
    out.append("// ...A's signer went first: B's pass completes the set\n")
    out.append(cstr("SPS_RA_B64", r_a.to_string()))
    out.append("// ...both shares and the derived script: the second visit, to sign\n")
    out.append(cstr("SPS_RAB_B64", r_ab.to_string()))
    out.append("// the proof this firmware must write for each request above: BIP374\n")
    out.append("// GenerateProof by embit, with the aux the firmware derives for that PSBT\n")
    out.append(carr("SPS_A_PROOF_R0", proof_of(A.secret, A.kiss_aux(r0.serialize()))))
    out.append(carr("SPS_A_PROOF_RB", proof_of(A.secret, A.kiss_aux(r_b.serialize()))))
    out.append(carr("SPS_B_PROOF_R0", proof_of(B.secret, B.kiss_aux(r0.serialize()))))
    out.append(carr("SPS_B_PROOF_RA", proof_of(B.secret, B.kiss_aux(r_a.serialize()))))

    # ---- refusals, as wallet A's signer sees them ----
    out.append("\n// ---- refusals (loaded by wallet A's signer) ----\n")
    bad = bytearray(proof_b)
    bad[40] ^= 1
    neg = [
        ("BADPROOF", "B's proof does not verify",
         put_share(base(), 1, share_b, bytes(bad))),
        ("WRONGKEY", "a valid share and proof, made with a key that is not input 1's",
         put_share(base(), 1, share_a, proof_a)),
        ("NOPROOF", "B's share without its proof", put_share(base(), 1, share_b, None)),
        ("NOSHARE", "B's proof without its share", put_share(base(), 1, None, proof_b)),
        ("EARLYSCRIPT", "the script is set while B's share is still missing",
         base(sp_spk=script.Script(spk))),
        ("WRONGSCRIPT", "every share is there and the script is not what they derive",
         put_share(base(sp_spk=script.p2tr(ec.PrivateKey(bytes([0x33] * 32)).get_public_key())),
                   1, share_b, proof_b)),
    ]
    g = put_share(base(), 1, share_b, proof_b)
    g.sp_ecdh_shares[scan] = share_sum
    g.sp_dleq_proofs[scan] = proof_of(
        ((int.from_bytes(A.secret, "big") + int.from_bytes(B.secret, "big"))
         % SECP256K1_ORDER).to_bytes(32, "big"), bytes(32))
    neg.append(("GLOBAL", "a global share beside an input that is not ours", g))

    # a foreign input that is not P2WPKH or P2TR: B pays from a legacy address
    prev_legacy = prev_tx(0xB3, [(IN_B, script.p2pkh(B.pub))])
    legacy = SPInputScope()
    legacy.txid = prev_legacy.txid()
    legacy.vout = 0
    legacy.sequence = 0xFFFFFFFD
    legacy.non_witness_utxo = prev_legacy
    legacy.bip32_derivations[B.pub] = DerivationPath(B.fp, INPUT_PATH)
    neg.append(("INELIGIBLE", "B's input is P2PKH", base(in_b=legacy)))
    neg.append(("NOKEY", "B's P2WPKH input names no key hashing to its program",
                put_share(base(in_b=wpkh_input(B, prev_b, 1, with_key=False)),
                          1, share_b, proof_b)))
    neg.append(("UNPROVEN", "B's input carries no previous transaction",
                put_share(base(in_b=wpkh_input(B, prev_b, 1, with_prev=False)),
                          1, share_b, proof_b)))
    lie = wpkh_input(B, prev_b, 1)
    lie.non_witness_utxo = prev_tx(0xB4, [(7_000, stranger), (IN_B, script.p2wpkh(B.pub))])
    neg.append(("PREVMISMATCH", "B's previous transaction does not hash to its outpoint",
                put_share(base(in_b=lie), 1, share_b, proof_b)))
    sh = base()
    sh.inputs[1].sighash_type = 3
    neg.append(("SIGHASH", "B's input asks for SIGHASH_SINGLE", sh))
    for name, what, psbt in neg:
        out.append("// %s\n" % what)
        out.append(cstr("SPS_N_%s_B64" % name, psbt.to_string()))

    # no silent payment output at all: a stranger's input stays a refusal
    plain = assemble(
        [wpkh_input(A, prev_a, 0), wpkh_input(B, prev_b, 1)],
        [change_output(A, CHANGE_A), change_output(B, IN_A + IN_B - CHANGE_A - 400)],
    )
    for o in plain.outputs:
        o.sp_data = None
    out.append("// the same two inputs with NO silent payment output: nothing to share\n")
    out.append(cstr("SPS_N_NOSP_B64", plain.to_string()))

    # a share planted on OUR input (a real point, B's, with B's proof): wiped
    out.append("// B's share and proof planted on A's input as well: never trusted\n")
    out.append(cstr("SPS_PLANTED_B64",
                    put_share(put_share(base(), 1, share_b, proof_b),
                              0, share_b, proof_b).to_string()))

    # ---- taproot: the even Y rule, on a foreign input and on one of ours ----
    out.append("\n// ---- taproot inputs (BIP352 counts the even Y key) ----\n")

    def odd_key(seed):
        for b in range(seed, 256):
            k = ec.PrivateKey(bytes([b] * 32))
            if k.get_public_key().sec()[0] == 0x03:
                return k
        raise AssertionError("no odd Y key")

    def even_scalar(key):
        d = int.from_bytes(key.secret, "big")
        if key.get_public_key().sec()[0] == 0x03:
            d = SECP256K1_ORDER - d
        return d.to_bytes(32, "big")

    def tr_input(key, marker, value):
        spk_tr = script.Script(b"\x51\x20" + key.get_public_key().xonly())
        prev = prev_tx(marker, [(value, spk_tr)])
        inp = SPInputScope()
        inp.txid = prev.txid()
        inp.vout = 0
        inp.sequence = 0xFFFFFFFD
        inp.witness_utxo = prev.vout[0]
        inp.non_witness_utxo = prev
        return inp

    # (1) A's P2WPKH coin beside B's taproot coin whose key has ODD Y
    b_tr = odd_key(0x61)
    b_tr_even = even_scalar(b_tr)
    ftr = assemble(
        [wpkh_input(A, prev_a, 0), tr_input(b_tr, 0xC1, IN_B)],
        [sp_output(), change_output(A, CHANGE_A), change_output(B, CHANGE_B)],
    )
    ftr_spk, _, _ = expected_script([A.secret, b_tr_even], ftr)
    put_share(ftr, 1, share_of(b_tr_even), proof_of(b_tr_even, bytes(32)))
    out.append("// A's P2WPKH input and a foreign P2TR input whose key has odd Y\n")
    out.append(cstr("SPS_FTR_B64", ftr.to_string()))
    out.append(carr("SPS_FTR_SCRIPT", ftr_spk))
    # the same with the share made from the key as it stands (odd Y): the proof
    # then answers for the wrong point and must not be accepted
    ftr_bad = assemble(
        [wpkh_input(A, prev_a, 0), tr_input(b_tr, 0xC1, IN_B)],
        [sp_output(), change_output(A, CHANGE_A), change_output(B, CHANGE_B)],
    )
    put_share(ftr_bad, 1, share_of(b_tr.secret), proof_of(b_tr.secret, bytes(32)))
    out.append("// ...and its share made without the even Y rule\n")
    out.append(cstr("SPS_N_FTR_ODD_B64", ftr_bad.to_string()))

    # (2) a received silent payment of A's (BIP376, odd Y) beside B's taproot
    #     coin. Every input is taproot, so BIP341 covers every amount.
    spend_priv = A.root.derive("m/352h/1h/0h/0h/0").key
    tweak = None
    for b in range(1, 256):
        if spend_priv.sp_spend_tweak(bytes([b] * 32)).sec()[0] == 0x03:
            tweak = bytes([b] * 32)
            break
    a_tr = spend_priv.sp_spend_tweak(tweak)
    a_tr_even = even_scalar(a_tr)
    own_tr = SPInputScope()
    own_tr.txid = bytes([0xCD] * 32)
    own_tr.vout = 0
    own_tr.sequence = 0xFFFFFFFD
    own_tr.witness_utxo = TransactionOutput(
        IN_A, script.Script(b"\x51\x20" + a_tr.get_public_key().xonly()))
    own_tr.sp_tweak = tweak
    own_tr.sp_spend_bip32_derivations[spend_priv.get_public_key().sec()] = DerivationPath(
        A.fp, bip32.parse_path("m/352h/1h/0h/0h/0"))
    btr = assemble(
        [own_tr, tr_input(b_tr, 0xC1, IN_B)],
        [sp_output(), change_output(A, CHANGE_A), change_output(B, CHANGE_B)],
    )
    btr_spk, _, _ = expected_script([a_tr_even, b_tr_even], btr)
    put_share(btr, 1, share_of(b_tr_even), proof_of(b_tr_even, bytes(32)))
    out.append("// a received silent payment of A's (BIP376, odd Y) and a foreign P2TR input\n")
    out.append(cstr("SPS_OTR_B64", btr.to_string()))
    out.append(carr("SPS_OTR_SCRIPT", btr_spk))
    out.append(carr("SPS_OTR_A_SHARE", share_of(a_tr_even)))
    out.append(carr("SPS_OTR_A_OUTKEY", a_tr.get_public_key().xonly()))

    # ---- two silent payment outputs with different scan keys ----
    scan2 = ec.PrivateKey(bytes([0x21] * 32)).get_public_key()
    spend2 = ec.PrivateKey(bytes([0x22] * 32)).get_public_key()
    two = assemble(
        [wpkh_input(A, prev_a, 0), wpkh_input(B, prev_b, 1)],
        [sp_output(value=12_000), change_output(A, CHANGE_A), change_output(B, CHANGE_B),
         sp_output(value=8_000)],
    )
    two.outputs[3].sp_data = SilentPaymentData(scan2, spend2)
    groups, _ = group_sp_outputs_by_scan_key(two.outputs)
    _, _, res2 = derive_sp_outputs([A.secret, B.secret], [i.vin for i in two.inputs], groups)

    def share2(secret):
        point = bytearray(ec_pubkey_parse(scan2.sec()))
        ec_pubkey_tweak_mul(point, secret)
        return ec_pubkey_serialize(point)

    put_share(two, 1, share_b, proof_b)                 # B: first scan key only
    out.append("\n// ---- two silent payment outputs, two scan keys ----\n")
    out.append("// B's share is there for the first scan key and missing for the second\n")
    out.append(cstr("SPS_TWO_HALF_B64", two.to_string()))
    two.inputs[1].unknown[b"\x1d" + scan2.sec()] = share2(B.secret)
    two.inputs[1].unknown[b"\x1e" + scan2.sec()] = dleq.generate_dleq_proof(
        B.secret, scan2.sec(), r=bytes(32))
    out.append("// ...and there for both\n")
    out.append(cstr("SPS_TWO_B64", two.to_string()))
    out.append(carr("SPS_TWO_SCAN2", scan2.sec()))
    out.append(carr("SPS_TWO_A_SHARE2", share2(A.secret)))
    out.append(carr("SPS_TWO_SCRIPT0", b"\x51\x20" + res2[scan][1][0]))
    out.append(carr("SPS_TWO_SCRIPT3", b"\x51\x20" + res2[scan2.sec()][1][0]))

    # ---- BIP375 test vectors: a third implementation's per input shares ----
    # Read straight off the PSBT bytes, not through embit: what a vector
    # carries is then what lands in the header, whatever a PSBT library makes
    # of fields it models or does not.
    if bip375:
        import base64

        vecs = json.load(open(bip375))

        def read_maps(raw):
            """[{raw key: value}] for the global map, then every input and
            output map in order."""
            def compact(at):
                c = raw[at]
                if c < 0xFD:
                    return c, at + 1
                width = {0xFD: 2, 0xFE: 4, 0xFF: 8}[c]
                return int.from_bytes(raw[at + 1:at + 1 + width], "little"), at + 1 + width

            maps, at = [], 5
            while at < len(raw):
                m = {}
                while True:
                    klen, at = compact(at)
                    if klen == 0:
                        break
                    key = raw[at:at + klen]
                    at += klen
                    vlen, at = compact(at)
                    m[key] = raw[at:at + vlen]
                    at += vlen
                maps.append(m)
            return maps

        def pick(kind, desc):
            hit = [v for v in vecs[kind] if v["description"] == desc]
            if not hit:
                sys.exit("BIP375 vector %r not found" % desc)
            maps = read_maps(base64.b64decode(hit[0]["psbt"]))
            n_in = maps[0][b"\x04"][0]
            return maps[1:1 + n_in], maps[1 + n_in:]

        def one(m, keytype):
            hit = [(k[1:], v) for k, v in m.items() if k[0] == keytype and len(k) == 34]
            if len(hit) != 1:
                sys.exit("BIP375 vector: expected exactly one 0x%02x field" % keytype)
            return hit[0]

        out.append("\n// ---- BIP375 test vectors, verbatim: per input shares ----\n")
        out.append("#define SPS_HAVE_BIP375 1\n")
        ins, outs = pick("valid", BIP375_PER_INPUT)
        out.append("// %s\n" % BIP375_PER_INPUT)
        out.append("// The vector's witness programs are not the HASH160 of these keys, so it\n")
        out.append("// is a test of the arithmetic and could never be loaded as a payment.\n")
        out.append("#define SPS375_NIN %d\n" % len(ins))
        out.append(carr("sps375_scan", one(ins[0], 0x1D)[0]))
        out.append(carr("sps375_spend", outs[0][b"\x09"][33:]))
        out.append(carr("sps375_pubs", b"".join(one(m, 0x06)[0] for m in ins)))
        out.append(carr("sps375_shares", b"".join(one(m, 0x1D)[1] for m in ins)))
        out.append(carr("sps375_proofs", b"".join(one(m, 0x1E)[1] for m in ins)))
        out.append(carr("sps375_outpoints",
                        b"".join(m[b"\x0e"] + m[b"\x0f"] for m in ins)))
        out.append(carr("sps375_script", outs[0][b"\x04"]))
        ins, outs = pick("invalid", BIP375_BAD_PROOF)
        out.append("// %s\n" % BIP375_BAD_PROOF)
        out.append(carr("sps375_bad_pub", one(ins[0], 0x06)[0]))
        out.append(carr("sps375_bad_share", one(ins[0], 0x1D)[1]))
        out.append(carr("sps375_bad_proof", one(ins[0], 0x1E)[1]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bips", type=Path)
    ap.add_argument("--embit", type=Path)
    ap.add_argument("--shared", action="store_true",
                    help="write sim/sp_shared_vectors.h and nothing else")
    ap.add_argument("--bip375", type=Path,
                    help="with --shared: bip375_test_vectors.json")
    args = ap.parse_args()
    if args.embit:
        sys.path.insert(0, str(args.embit))

    if args.shared:
        out = []
        build_shared(out, args.bip375)
        OUT_SHARED.write_text("".join(out))
        print("wrote %s (%d lines)" % (OUT_SHARED, "".join(out).count("\n")))
        return
    if not args.bips or not args.embit:
        ap.error("--bips and --embit are required (or --shared)")

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
