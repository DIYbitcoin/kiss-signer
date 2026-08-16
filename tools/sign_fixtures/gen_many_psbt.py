#!/usr/bin/env python3
"""Build a testnet PSBT that pays several recipients, for the SD card.

The multi-recipient sign screen is the one shape the simulator cannot test on
hardware: sim/sim_main.c fakes its `zzzz-MANY.psbt` by matching the bytes
"MANY", so that file will not parse on a device. This produces a real one.

It has to be YOURS. kiss_psbt.c re-derives the scriptPubKey of every input from
the master key and refuses anything that does not match ("input script does not
re-derive"), so a PSBT built against somebody else's key STOPs before the sign
screen it is meant to show. You supply one of your own receive addresses and its
index; the input spends that.

    tools/sign_fixtures/venv/bin/python tools/sign_fixtures/gen_many_psbt.py \\
        --fp 12A4BB6B --addr tb1q... --index 0 --out /Volumes/KISS/many.psbt

Read the fingerprint off the home screen or the SIGN header, and the address and
its index off RECEIVE. Defaults assume Native SegWit on testnet, m/84h/1h/0h/0/i;
pass --purpose 49 or 44 if Settings says otherwise.

The previous transaction is fabricated and included in full, which is what makes
the amounts PROVEN and the screen caution-free: kiss_psbt.c only requires that
the prev tx hashes to the outpoint being spent, and both sides are built here.
It is not on any chain. This is for looking at the screen, never for broadcast,
and it is testnet regardless.
"""
import argparse
import sys

try:
    from embit import script
    from embit.networks import NETWORKS
    from embit.psbt import PSBT, DerivationPath
    from embit.transaction import Transaction, TransactionInput, TransactionOutput
    from embit.ec import PublicKey
except ImportError:
    sys.exit("need embit: python3 -m venv tools/sign_fixtures/venv && "
             "tools/sign_fixtures/venv/bin/pip install embit")

# Five destinations, so the output column scrolls and the read-to-the-end gate
# on HOLD TO SIGN has something to hold for. Valid P2WPKH, and nobody's in
# particular: the hash160s are all-ones through all-fives, which no key produces.
def _p2wpkh_spk(h160):
    return script.Script(b"\x00\x14" + h160)


RECIPIENTS = [_p2wpkh_spk(bytes([i + 1]) * 20) for i in range(5)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fp", required=True, help="master fingerprint, 8 hex chars")
    ap.add_argument("--addr", required=True, help="one of YOUR receive addresses")
    ap.add_argument("--index", type=int, default=0, help="its index on RECEIVE")
    ap.add_argument("--purpose", type=int, default=84, choices=(44, 49, 84))
    ap.add_argument("--in-sats", type=int, default=250_000)
    ap.add_argument("--each", type=int, default=40_000, help="sats per recipient")
    ap.add_argument("--fee", type=int, default=2_000)
    ap.add_argument("--out", required=True, help="where to write the .psbt")
    a = ap.parse_args()

    fp = bytes.fromhex(a.fp)
    if len(fp) != 4:
        sys.exit("--fp must be 8 hex characters")

    spk = script.Script(script.address_to_scriptpubkey(a.addr).data)
    spent = a.each * len(RECIPIENTS) + a.fee
    change = a.in_sats - spent
    if change < 0:
        sys.exit(f"inputs {a.in_sats} cannot cover {spent}")

    # The previous transaction, built so its txid IS the outpoint we spend.
    prev = Transaction(
        vin=[TransactionInput(b"\x11" * 32, 0)],
        vout=[TransactionOutput(a.in_sats, spk)],
    )

    tx = Transaction(
        # embit's txid() is already the internal byte order a TransactionInput
        # takes. Reversing it here is what produced "input's previous
        # transaction does not match" -- kiss_psbt.c hashes the prev tx and
        # compares, so the two have to be the same 32 bytes the same way up.
        vin=[TransactionInput(prev.txid(), 0, sequence=0xFFFFFFFD)],
        vout=([TransactionOutput(a.each, r) for r in RECIPIENTS]
              + ([TransactionOutput(change, spk)] if change else [])),
    )

    psbt = PSBT(tx)
    inp = psbt.inputs[0]
    inp.non_witness_utxo = prev              # proven amounts, no caution
    # The keypath map is what our_keypath() searches by fingerprint, and
    # rederive_matches() then checks the PATH against the real key -- so the
    # path has to be right and the pubkey beside it is never verified.
    path = [0x80000000 | a.purpose, 0x80000001, 0x80000000, 0, a.index]
    inp.bip32_derivations[PublicKey.parse(b"\x02" + b"\x01" * 32)] = DerivationPath(fp, path)
    if change:
        # The change output is re-derived too (rederive_matches, our_purpose),
        # so it is marked at the SAME address and path as the input: the one
        # place this file cannot invent a value.
        out = psbt.outputs[-1]
        out.bip32_derivations[PublicKey.parse(b"\x02" + b"\x01" * 32)] = DerivationPath(fp, path)

    data = psbt.serialize()
    with open(a.out, "wb") as f:
        f.write(data)
    print(f"wrote {a.out}: {len(data)} bytes, {len(RECIPIENTS)} recipients "
          f"x {a.each} sats, fee {a.fee}, change {change}")


if __name__ == "__main__":
    main()
